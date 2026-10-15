#include "transferworker.h"
#include "protocol.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSaveFile>
#include <QStorageInfo>
#include <QtEndian>

TransferWorker::TransferWorker(const SendRequest &request)
    : m_id(request.id), m_sender(true), m_request(request)
{
    m_file.setParent(this);
    m_hashFile.setParent(this);
}

TransferWorker::TransferWorker(const QString &id, qintptr descriptor, const QString &receiveDirectory, bool autoAccept)
    : m_id(id), m_sender(false), m_descriptor(descriptor), m_receiveDirectory(receiveDirectory), m_autoAccept(autoAccept)
{
    m_file.setParent(this);
    m_hashFile.setParent(this);
}

TransferWorker::~TransferWorker() = default;

void TransferWorker::createSocket()
{
    // The socket and timers are constructed AFTER moveToThread: all I/O stays in that thread.
    m_socket = new QTcpSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->setReadBufferSize(2 * Wire::MaxFrame);
    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    m_watchdog->setInterval(120000);
    connect(m_watchdog, &QTimer::timeout, this, [this] { fail(QStringLiteral("连接超时，可重新继续传输")); });
    m_keepAlive = new QTimer(this);
    m_keepAlive->setInterval(5000);
    connect(m_keepAlive, &QTimer::timeout, this, [this] {
        if (m_socket->state() == QAbstractSocket::ConnectedState)
            sendControl({{"type", "ping"}});
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &TransferWorker::readFrames);
    connect(m_socket, &QTcpSocket::bytesWritten, this, [this](qint64) {
        if (m_state == State::Sending && !m_pumpScheduled) {
            m_pumpScheduled = true;
            QTimer::singleShot(m_request.chunkDelayMs, this, [this] { m_pumpScheduled = false; pump(); });
        }
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // A final ACK may already be buffered when the remote closes its connection.
        if (m_socket->bytesAvailable())
            readFrames();
        if (m_state != State::Done)
            fail(QStringLiteral("网络中断：%1；可重新继续传输").arg(m_socket->errorString()), false);
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_socket->bytesAvailable())
            readFrames();
        if (m_state != State::Done)
            fail(QStringLiteral("对端已断开，可重新继续传输"), false);
    });
}

void TransferWorker::start()
{
    createSocket();
    if (m_sender) {
        prepareSend();
    } else {
        m_state = State::AwaitOffer;
        if (!m_socket->setSocketDescriptor(m_descriptor)) {
            fail(QStringLiteral("无法接管接收连接：%1").arg(m_socket->errorString()));
            return;
        }
        m_peer = m_socket->peerAddress().toString();
        m_watchdog->start();
        m_keepAlive->start();
        emit stateChanged(m_id, QStringLiteral("等待文件信息"));
        if (m_socket->bytesAvailable())
            readFrames();
    }
}

void TransferWorker::prepareSend()
{
    const QFileInfo info(m_request.path);
    m_name = info.fileName();
    m_size = info.size();
    m_sourceModified = info.lastModified();
    m_peer = QStringLiteral("%1:%2").arg(m_request.host).arg(m_request.port);
    emit metadata(m_id, m_name, m_peer, m_size);
    if (!info.isFile() || !Wire::safeFileName(m_name) || m_size < 0 || m_size > Wire::MaxFileSize) {
        fail(QStringLiteral("文件不存在、名称不合法或超过 1 TiB"));
        return;
    }
    m_file.setFileName(info.absoluteFilePath());
    if (!m_file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("无法读取文件：%1").arg(m_file.errorString()));
        return;
    }
    m_state = State::HashingSource;
    emit stateChanged(m_id, QStringLiteral("计算 SHA-256"));
    hashFile(m_file.fileName(), m_size, [this](const QString &digest) {
        if (!sourceUnchanged()) {
            fail(QStringLiteral("文件在校验期间发生变化，请重新发送"));
            return;
        }
        m_digest = digest;
        m_state = State::Connecting;
        emit stateChanged(m_id, QStringLiteral("连接对端"));
        connect(m_socket, &QTcpSocket::connected, this, [this] {
            m_state = State::AwaitReady;
            m_keepAlive->start();
            emit stateChanged(m_id, QStringLiteral("等待接收确认"));
            sendControl({{"type", "offer"}, {"app", "ByteMe"}, {"version", 1},
                         {"name", m_name}, {"size", QString::number(m_size)}, {"sha256", m_digest}});
        });
        m_watchdog->start();
        m_socket->connectToHost(m_request.host, m_request.port);
    });
}

void TransferWorker::readFrames()
{
    if (m_state == State::Done || m_readScheduled)
        return;
    int budget = 16;
    while (budget-- > 0 && m_state != State::Done) {
        // Keep application buffering bounded even if the peer floods us.
        const auto room = 2 * Wire::MaxFrame - m_buffer.size();
        if (room > 0 && m_socket->bytesAvailable()) {
            m_buffer.append(m_socket->read(room));
            m_watchdog->start();
        }
        Wire::Frame frame;
        QString error;
        const auto result = Wire::take(m_buffer, frame, error);
        if (result == Wire::ParseResult::Invalid) { fail(error); return; }
        if (result == Wire::ParseResult::Incomplete)
            break;
        if (frame.kind == Wire::Kind::Data) {
            handleData(frame.payload);
        } else {
            if (frame.payload.size() > 8192) { fail(QStringLiteral("控制消息过大")); return; }
            const auto document = QJsonDocument::fromJson(frame.payload);
            if (!document.isObject()) { fail(QStringLiteral("控制消息格式错误")); return; }
            handleControl(document.object());
        }
    }
    // Schedule another bounded batch only if a complete frame or unread socket data exists.
    bool complete = m_buffer.size() >= 4
        && quint32(m_buffer.size() - 4) >= qFromBigEndian<quint32>(m_buffer.constData());
    if (m_state != State::Done && (m_socket->bytesAvailable() || complete)) {
        m_readScheduled = true;
        QTimer::singleShot(0, this, [this] { m_readScheduled = false; readFrames(); });
    }
}

bool TransferWorker::sendControl(const QJsonObject &object)
{
    if (m_state == State::Done)
        return false;
    const auto bytes = Wire::control(object);
    if (m_socket->bytesToWrite() > 2 * 1024 * 1024 || m_socket->write(bytes) != bytes.size()) {
        fail(QStringLiteral("网络写入失败"));
        return false;
    }
    return true;
}

void TransferWorker::handleControl(const QJsonObject &object)
{
    const auto type = object.value("type").toString();
    if (type == "ping")
        return;
    if (type == "error") { fail(QStringLiteral("对端：%1").arg(object.value("message").toString().left(300))); return; }
    if (!m_sender && m_state == State::AwaitOffer && type == "offer") {
        acceptOffer(object);
        return;
    }
    if (m_sender && m_state == State::AwaitReady && type == "ready") {
        qint64 offset = 0;
        const QString prefix = object.value("prefixSha256").toString();
        if (!Wire::parseNumber(object, "offset", offset) || offset > m_size || !Wire::validDigest(prefix)) {
            fail(QStringLiteral("非法续传位置或校验值"));
            return;
        }
        m_resumeOffset = offset;
        m_state = State::HashingPrefix;
        emit stateChanged(m_id, QStringLiteral("校验续传片段"));
        hashFile(m_file.fileName(), offset, [this, prefix](const QString &actual) {
            if (!sourceUnchanged()) { fail(QStringLiteral("源文件已发生变化")); return; }
            if (actual != prefix) {
                m_state = State::AwaitReady;
                emit stateChanged(m_id, QStringLiteral("片段不一致，重新传输"));
                sendControl({{"type", "restart"}});
                return;
            }
            m_position = m_resumeOffset;
            if (!m_file.seek(m_position)) { fail(QStringLiteral("无法定位源文件")); return; }
            m_speedBaseline = m_position;
            m_speedClock.start();
            m_progressClock.start();
            if (!sendControl({{"type", "begin"}, {"offset", QString::number(m_position)}}))
                return;
            m_state = State::Sending;
            emit stateChanged(m_id, m_position ? QStringLiteral("续传中") : QStringLiteral("发送中"));
            publishProgress(true);
            pump();
        });
        return;
    }
    if (!m_sender && m_state == State::AwaitBegin && type == "restart") {
        if (!m_file.resize(0) || !m_file.seek(0)) { fail(QStringLiteral("无法重置损坏片段")); return; }
        m_position = 0;
        m_prefixDigest = QString::fromLatin1(QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
        sendReady();
        return;
    }
    if (!m_sender && m_state == State::AwaitBegin && type == "begin") {
        qint64 offset = 0;
        if (!Wire::parseNumber(object, "offset", offset) || offset != m_position) {
            fail(QStringLiteral("续传位置不一致"));
            return;
        }
        m_state = State::Receiving;
        m_speedBaseline = m_position;
        m_speedClock.start();
        m_progressClock.start();
        emit stateChanged(m_id, m_position ? QStringLiteral("续传接收中") : QStringLiteral("接收中"));
        publishProgress(true);
        return;
    }
    if (!m_sender && m_state == State::Receiving && type == "finish") {
        if (m_position != m_size) { fail(QStringLiteral("文件尚未完整接收")); return; }
        verifyAndCommit();
        return;
    }
    if (m_sender && m_state == State::AwaitComplete && type == "complete") {
        if (object.value("sha256").toString() != m_digest) { fail(QStringLiteral("最终校验值不一致")); return; }
        publishProgress(true);
        finish(true, QStringLiteral("发送完成，对端 SHA-256 校验通过"));
        return;
    }
    fail(QStringLiteral("协议状态错误：%1").arg(type.left(40)));
}

void TransferWorker::acceptOffer(const QJsonObject &object)
{
    m_name = object.value("name").toString();
    m_digest = object.value("sha256").toString();
    if (object.value("app") != "ByteMe" || object.value("version").toInt() != 1
        || !Wire::safeFileName(m_name) || !Wire::validDigest(m_digest)
        || !Wire::parseNumber(object, "size", m_size)) {
        fail(QStringLiteral("文件信息不合法"));
        return;
    }
    emit metadata(m_id, m_name, m_peer, m_size);
    m_state = State::AwaitApproval;
    emit stateChanged(m_id, QStringLiteral("等待本机确认"));
    if (m_autoAccept)
        approve(true);
    else
        emit offered(m_id, m_name, m_peer, m_size);
}

void TransferWorker::approve(bool accepted)
{
    if (m_state != State::AwaitApproval)
        return;
    if (!accepted) { fail(QStringLiteral("接收方拒绝文件")); return; }
    prepareReceive();
}

void TransferWorker::prepareReceive()
{
    QDir directory(m_receiveDirectory);
    if (!directory.exists() && !QDir().mkpath(m_receiveDirectory)) {
        fail(QStringLiteral("无法创建接收目录"));
        return;
    }
    const auto partialDirectory = directory.filePath(QStringLiteral(".byteme-partials"));
    if (QFileInfo(partialDirectory).isSymLink() || !QDir().mkpath(partialDirectory)) {
        fail(QStringLiteral("续传目录不可用"));
        return;
    }
    const auto key = Wire::storageKey(m_name, m_size, m_digest);
    m_partPath = QDir(partialDirectory).filePath(key + ".part");
    m_metaPath = QDir(partialDirectory).filePath(key + ".json");
    m_lock = std::make_unique<QLockFile>(QDir(partialDirectory).filePath(key + ".lock"));
    if (!m_lock->tryLock(0)) { fail(QStringLiteral("同一文件正在接收，请稍后继续")); return; }
    if (QFileInfo(m_partPath).isSymLink() || QFileInfo(m_metaPath).isSymLink()) {
        fail(QStringLiteral("续传文件路径不可用"));
        return;
    }
    const QJsonObject expected{{"version", 1}, {"name", m_name},
                               {"size", QString::number(m_size)}, {"sha256", m_digest}};
    const bool hasMetadata = QFileInfo::exists(m_metaPath);
    if (hasMetadata) {
        QFile metadataFile(m_metaPath);
        if (!metadataFile.open(QIODevice::ReadOnly) || metadataFile.size() > 8192
            || QJsonDocument::fromJson(metadataFile.readAll()).object() != expected) {
            fail(QStringLiteral("续传记录损坏，请清理对应的临时文件后重试"));
            return;
        }
    } else {
        QSaveFile metadataFile(m_metaPath);
        const auto bytes = QJsonDocument(expected).toJson(QJsonDocument::Compact);
        if (!metadataFile.open(QIODevice::WriteOnly) || metadataFile.write(bytes) != bytes.size()
            || !metadataFile.commit()) {
            fail(QStringLiteral("无法保存续传记录"));
            return;
        }
    }
    m_file.setFileName(m_partPath);
    if (!m_file.open(QIODevice::ReadWrite)) { fail(QStringLiteral("无法写入临时文件：%1").arg(m_file.errorString())); return; }
    if ((!hasMetadata || m_file.size() > m_size) && !m_file.resize(0)) {
        fail(QStringLiteral("无法重置临时文件"));
        return;
    }
    m_position = m_file.size();
    const QStorageInfo storage(directory.absolutePath());
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() < m_size - m_position) {
        fail(QStringLiteral("接收磁盘空间不足"));
        return;
    }
    if (!m_file.seek(m_position)) { fail(QStringLiteral("无法定位临时文件")); return; }
    m_state = State::HashingPrefix;
    emit stateChanged(m_id, QStringLiteral("检查已接收片段"));
    hashFile(m_partPath, m_position, [this](const QString &digest) {
        m_prefixDigest = digest;
        sendReady();
    });
}

void TransferWorker::sendReady()
{
    m_state = State::AwaitBegin;
    emit stateChanged(m_id, QStringLiteral("等待传输数据"));
    sendControl({{"type", "ready"}, {"offset", QString::number(m_position)}, {"prefixSha256", m_prefixDigest}});
}

void TransferWorker::handleData(const QByteArray &data)
{
    if (m_sender || m_state != State::Receiving || data.isEmpty() || data.size() > Wire::ChunkSize
        || data.size() > m_size - m_position) {
        fail(QStringLiteral("非法数据块或数据超过文件大小"));
        return;
    }
    if (m_file.write(data) != data.size()) { fail(QStringLiteral("磁盘写入失败：%1").arg(m_file.errorString())); return; }
    m_position += data.size();
    publishProgress();
}

void TransferWorker::pump()
{
    if (m_state != State::Sending)
        return;
    int budget = m_request.chunkDelayMs ? 1 : 8;
    while (budget-- > 0 && m_socket->bytesToWrite() < 512 * 1024 && m_position < m_size) {
        const auto data = m_file.read(qMin<qint64>(Wire::ChunkSize, m_size - m_position));
        if (data.isEmpty()) { fail(QStringLiteral("读取源文件失败或文件发生变化")); return; }
        const auto frame = Wire::pack(Wire::Kind::Data, data);
        if (m_socket->write(frame) != frame.size()) { fail(QStringLiteral("网络写入失败")); return; }
        m_position += data.size();
        publishProgress();
    }
    if (m_position == m_size) {
        if (!sourceUnchanged()) { fail(QStringLiteral("源文件在传输期间发生变化")); return; }
        m_state = State::AwaitComplete;
        emit stateChanged(m_id, QStringLiteral("等待对端完整校验"));
        sendControl({{"type", "finish"}});
    }
}

void TransferWorker::verifyAndCommit()
{
    if (!m_file.flush()) { fail(QStringLiteral("无法保存接收数据")); return; }
    m_state = State::Verifying;
    emit stateChanged(m_id, QStringLiteral("校验完整文件"));
    publishProgress(true);
    hashFile(m_partPath, m_size, [this](const QString &digest) {
        if (digest != m_digest) { fail(QStringLiteral("SHA-256 校验失败，下次继续时将检查并重传")); return; }
        m_file.close();
        commitReceived();
    });
}

void TransferWorker::commitReceived()
{
    if (m_state != State::Verifying)
        return;
    // Serialize final-name selection across threads and cooperating app instances.
    // Per-content locks alone do not protect two different files with the same name.
    QLockFile commitLock(QDir(m_receiveDirectory).filePath(".byteme-partials/commit.lock"));
    if (!commitLock.tryLock(0)) {
        if (commitLock.error() != QLockFile::LockFailedError) {
            fail(QStringLiteral("无法锁定接收目录"));
            return;
        }
        QTimer::singleShot(20, this, &TransferWorker::commitReceived);
        return;
    }
    const QDir directory(m_receiveDirectory);
    QString target = directory.filePath(m_name);
    const QFileInfo original(m_name);
    for (int index = 0; index < 10000; ++index) {
        if (index) {
            const auto suffix = original.suffix();
            target = directory.filePath(QStringLiteral("%1 (%2)%3")
                .arg(original.completeBaseName()).arg(index)
                .arg(suffix.isEmpty() ? QString() : "." + suffix));
        }
        if (QFileInfo::exists(target))
            continue;
        if (!QFile::rename(m_partPath, target)) {
            if (QFileInfo::exists(target))
                continue;
            fail(QStringLiteral("无法将临时文件保存到接收目录"));
            return;
        }
        QFile::remove(m_metaPath);
        m_lock->unlock();
        emit stateChanged(m_id, QStringLiteral("接收完成"));
        if (!sendControl({{"type", "complete"}, {"sha256", m_digest}}))
            return;
        // Finish after ACK is drained so the sender can distinguish verified success.
        auto complete = [this, target] {
            if (m_state != State::Done && m_socket->bytesToWrite() == 0)
                finish(true, QStringLiteral("接收完成，SHA-256 校验通过"), target);
        };
        connect(m_socket, &QTcpSocket::bytesWritten, this, [complete](qint64) { complete(); });
        complete();
        return;
    }
    fail(QStringLiteral("同名文件过多，无法保存"));
}

void TransferWorker::hashFile(const QString &path, qint64 length, std::function<void(const QString &)> callback)
{
    m_hashFile.close();
    m_hashFile.setFileName(path);
    if (!m_hashFile.open(QIODevice::ReadOnly)) { fail(QStringLiteral("无法打开校验文件")); return; }
    m_hash.reset();
    m_hashRemaining = length;
    m_hashCallback = std::move(callback);
    QTimer::singleShot(0, this, &TransferWorker::hashStep);
}

void TransferWorker::hashStep()
{
    if (m_state == State::Done || !m_hashCallback)
        return;
    if (m_hashRemaining > 0) {
        const auto data = m_hashFile.read(qMin<qint64>(256 * 1024, m_hashRemaining));
        if (data.isEmpty()) { fail(QStringLiteral("校验读取失败，文件可能发生变化")); return; }
        m_hash.addData(data);
        m_hashRemaining -= data.size();
        QTimer::singleShot(0, this, &TransferWorker::hashStep);
        return;
    }
    const auto digest = QString::fromLatin1(m_hash.result().toHex());
    m_hashFile.close();
    auto callback = std::move(m_hashCallback);
    m_hashCallback = {};
    callback(digest);
}

bool TransferWorker::sourceUnchanged() const
{
    const QFileInfo info(m_file.fileName());
    return info.isFile() && info.size() == m_size && info.lastModified() == m_sourceModified;
}

void TransferWorker::publishProgress(bool force)
{
    if (!force && m_progressClock.isValid() && m_progressClock.elapsed() < 100)
        return;
    const auto elapsed = m_speedClock.isValid() ? m_speedClock.elapsed() : 0;
    const double speed = elapsed > 0 ? double(m_position - m_speedBaseline) * 1000.0 / double(elapsed) : 0;
    emit progress(m_id, m_position, m_size, speed);
    m_progressClock.restart();
}

void TransferWorker::stop()
{
    if (m_state != State::Done)
        finish(false, QStringLiteral("已暂停，临时文件已保留"));
}

void TransferWorker::fail(const QString &message, bool notifyPeer)
{
    if (m_state == State::Done)
        return;
    if (notifyPeer && m_socket && m_socket->isValid() && m_socket->state() == QAbstractSocket::ConnectedState) {
        // Best effort error notification. Resume never relies on this message being delivered.
        m_socket->write(Wire::control({{"type", "error"}, {"message", message.left(300)}}));
    }
    finish(false, message);
}

void TransferWorker::finish(bool success, const QString &message, const QString &path)
{
    if (m_state == State::Done)
        return;
    m_state = State::Done;
    m_hashCallback = {};
    if (m_watchdog) m_watchdog->stop();
    if (m_keepAlive) m_keepAlive->stop();
    m_hashFile.close();
    m_file.close(); // Flush before releasing the lock, preserving the exact resume length.
    if (m_lock) m_lock->unlock();
    // Defer socket teardown until a readyRead/error callback has fully unwound.
    // Closing it reentrantly can invalidate Windows' active socket notifier.
    QTimer::singleShot(0, this, [this, success, message, path] {
        if (m_socket) {
            if (success) m_socket->disconnectFromHost();
            else m_socket->abort();
        }
        emit finished(m_id, success, message, path);
    });
}
