#include "discovery.h"
#include "protocol.h"
#include "transfermanager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <memory>

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool finished(const QSignalSpy &spy, const QString &id, bool success)
{
    for (const auto &event : spy)
        if (event.at(0).toString() == id && event.at(1).toBool() == success) return true;
    return false;
}
QByteArray sampleData(int size)
{
    QByteArray bytes(size, '\0');
    for (int index = 0; index < size; ++index)
        bytes[index] = char((index * 17 + index / 251) % 256);
    return bytes;
}
QJsonObject offer(const QString &name, qint64 size, const QString &digest)
{
    return {{"type", "offer"}, {"app", "ByteMe"}, {"version", 1}, {"name", name},
            {"size", QString::number(size)}, {"sha256", digest}};
}
}

class Integration final : public QObject {
    Q_OBJECT
private slots:
    void framing();
    void fileNames();
    void discovery();
    void transfer_data();
    void transfer();
    void concurrentAndCollision();
    void persistentResume();
    void corruptPartialRestarts();
    void rejectOffer();
    void invalidOffer_data();
    void invalidOffer();
    void oversizeFrame();
    void incompleteTransferIsNeverCommitted();
    void checksumMismatchIsNeverCommitted();
    void shutdownDuringTransfer();
};

void Integration::framing()
{
    const auto control = Wire::control({{"type", "ping"}});
    const auto data = Wire::pack(Wire::Kind::Data, QByteArray("a\0b", 3));
    QByteArray buffer;
    QString error;
    Wire::Frame frame;
    for (int index = 0; index < control.size() - 1; ++index) {
        buffer += control.at(index);
        QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Incomplete);
    }
    buffer += control.back();
    buffer += data;
    QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Complete);
    QCOMPARE(frame.kind, Wire::Kind::Control);
    QCOMPARE(QJsonDocument::fromJson(frame.payload).object().value("type").toString(), QString("ping"));
    QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Complete);
    QCOMPARE(frame.payload, QByteArray("a\0b", 3));
    QVERIFY(buffer.isEmpty());
    buffer.resize(4);
    qToBigEndian<quint32>(Wire::MaxFrame + 1, buffer.data());
    QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Invalid);
    qToBigEndian<quint32>(0, buffer.data());
    QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Invalid);
    buffer = QByteArray::fromHex("0000000163");
    QCOMPARE(Wire::take(buffer, frame, error), Wire::ParseResult::Invalid);
    qint64 value = -1;
    QVERIFY(!Wire::parseNumber({{"size", 3}}, "size", value));
    QVERIFY(!Wire::parseNumber({{"size", "-1"}}, "size", value));
}

void Integration::fileNames()
{
    QVERIFY(Wire::safeFileName(QStringLiteral("实验报告 2026.pdf")));
    for (const auto &name : {QString("../escape"), QString("..\\escape"), QString("CON.txt"),
                            QString("LPT1"), QString("bad:"), QString("trail."), QString("trail "), QString("..")})
        QVERIFY2(!Wire::safeFileName(name), qPrintable(name));
    QVERIFY(Wire::validDigest(QString(64, 'a')));
    QVERIFY(!Wire::validDigest(QString(64, 'z')));
}

void Integration::discovery()
{
    // Distinct ports avoid OS-dependent delivery between UDP sockets sharing one port.
    QUdpSocket reservationA, reservationB;
    QVERIFY(reservationA.bind(QHostAddress::LocalHost, 0));
    QVERIFY(reservationB.bind(QHostAddress::LocalHost, 0));
    const auto portA = reservationA.localPort(), portB = reservationB.localPort();
    reservationA.close(); reservationB.close();
    Discovery first, second;
    QSignalSpy seenSecond(&second, &Discovery::peerSeen);
    QVERIFY(first.start("first", QStringLiteral("设备一"), 50101, portA));
    QVERIFY(second.start("second", QStringLiteral("设备二"), 50102, portB));
    QUdpSocket probe;
    const auto packet = QJsonDocument(QJsonObject{{"app", "ByteMe"}, {"version", 1}, {"type", "hello"},
        {"id", "first"}, {"name", QStringLiteral("设备一")}, {"port", 50101}}).toJson(QJsonDocument::Compact);
    // A loopback probe receives the unicast alive response on its own ephemeral port.
    QVERIFY(probe.writeDatagram(packet, QHostAddress::LocalHost, portB) > 0);
    QTRY_VERIFY_WITH_TIMEOUT(!seenSecond.isEmpty(), 3000);
    const auto peer = qvariant_cast<Peer>(seenSecond.first().first());
    QCOMPARE(peer.id, QString("first"));
    QCOMPARE(peer.port, quint16(50101));
    QCOMPARE(peer.address, QHostAddress(QHostAddress::LocalHost));
    QTRY_VERIFY_WITH_TIMEOUT(probe.hasPendingDatagrams(), 3000);
    const auto response = QJsonDocument::fromJson(probe.receiveDatagram().data()).object();
    QCOMPARE(response.value("type").toString(), QString("alive"));
    QCOMPARE(response.value("id").toString(), QString("second"));
    const auto spoof = QJsonDocument(QJsonObject{{"app", "AnotherApp"}, {"version", 1}, {"type", "hello"},
        {"id", "spoof"}, {"name", "bad"}, {"port", 50101}}).toJson();
    probe.writeDatagram(spoof, QHostAddress::LocalHost, portB);
    QTest::qWait(100);
    QCOMPARE(seenSecond.size(), 1);
    first.stop(); second.stop();
}

void Integration::transfer_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<QString>("name");
    QTest::newRow("empty") << QByteArray() << QString("empty.bin");
    QTest::newRow("unicode") << sampleData(2 * 1024 * 1024 + 37) << QStringLiteral("中文文件 测试.bin");
    QTest::newRow("small") << QByteArray("hello\0world", 11) << QString("small.bin");
}

void Integration::transfer()
{
    QFETCH(QByteArray, data);
    QFETCH(QString, name);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(name);
    const QString destination = temporary.filePath("received");
    QVERIFY(writeFile(source, data));
    TransferManager receiver, sender;
    receiver.setReceiveDirectory(destination);
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy sent(&sender, &TransferManager::taskFinished);
    QSignalSpy received(&receiver, &TransferManager::taskFinished);
    const auto id = sender.sendFile(source, "127.0.0.1", receiver.port());
    QTRY_VERIFY_WITH_TIMEOUT(finished(sent, id, true), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 3000);
    QVERIFY(received.last().at(1).toBool());
    QVERIFY(QFileInfo::exists(QDir(destination).filePath(name)));
    QCOMPARE(readFile(QDir(destination).filePath(name)), data);
    QVERIFY(QDir(QDir(destination).filePath(".byteme-partials")).entryList({"*.part", "*.json"}, QDir::Files).isEmpty());
}

void Integration::concurrentAndCollision()
{
    QTemporaryDir temporary;
    const QString destination = temporary.filePath("received");
    QVERIFY(QDir().mkpath(destination));
    QVERIFY(writeFile(QDir(destination).filePath("same.bin"), "original"));
    TransferManager receiver, sender;
    receiver.setReceiveDirectory(destination);
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy results(&sender, &TransferManager::taskFinished);
    QSignalSpy received(&receiver, &TransferManager::taskFinished);
    QStringList ids;
    QSet<QByteArray> expected;
    for (int index = 0; index < 6; ++index) {
        const auto folder = temporary.filePath(QString::number(index));
        QVERIFY(QDir().mkpath(folder));
        const QByteArray payload = sampleData(256 * 1024) + QByteArray::number(index);
        expected.insert(QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
        const auto path = QDir(folder).filePath("same.bin");
        QVERIFY(writeFile(path, payload));
        ids.append(sender.sendFile(path, "127.0.0.1", receiver.port(), 2));
    }
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 6, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), 6, 3000);
    for (const auto &id : ids) QVERIFY(finished(results, id, true));
    QSet<QString> savedPaths;
    for (const auto &event : received) {
        QVERIFY(event.at(1).toBool());
        savedPaths.insert(event.at(3).toString());
    }
    QCOMPARE(savedPaths.size(), 6);
    QCOMPARE(readFile(QDir(destination).filePath("same.bin")), QByteArray("original"));
    const auto files = QDir(destination).entryList({"*.bin"}, QDir::Files);
    QCOMPARE(files.size(), 7);
    for (const auto &name : files) {
        if (name == "same.bin") continue;
        QVERIFY(expected.remove(QCryptographicHash::hash(readFile(QDir(destination).filePath(name)), QCryptographicHash::Sha256)));
    }
    QVERIFY(expected.isEmpty());
}

void Integration::persistentResume()
{
    QTemporaryDir temporary;
    const auto payload = sampleData(4 * 1024 * 1024);
    const auto source = temporary.filePath("resume.bin");
    const auto destination = temporary.filePath("received");
    QVERIFY(writeFile(source, payload));
    qint64 partialLength = 0;
    {
        TransferManager receiver, sender;
        receiver.setReceiveDirectory(destination);
        receiver.setAutoAccept(true);
        QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
        bool paused = false;
        connect(&sender, &TransferManager::taskProgress, &sender,
            [&](const QString &id, qint64 done, qint64, double) {
                if (!paused && done >= 512 * 1024) { paused = true; sender.pause(id); }
            });
        QSignalSpy result(&sender, &TransferManager::taskFinished);
        QSignalSpy remote(&receiver, &TransferManager::taskFinished);
        const auto id = sender.sendFile(source, "127.0.0.1", receiver.port(), 5);
        QTRY_VERIFY_WITH_TIMEOUT(finished(result, id, false), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!remote.isEmpty(), 3000);
        QVERIFY(paused);
        sender.shutdown(); receiver.shutdown();
    }
    const QDir parts(QDir(destination).filePath(".byteme-partials"));
    const auto files = parts.entryList({"*.part"}, QDir::Files);
    QCOMPARE(files.size(), 1);
    partialLength = QFileInfo(parts.filePath(files.first())).size();
    QVERIFY(partialLength > 0 && partialLength < payload.size());
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath("resume.bin")));
    // New managers simulate restarting both programs; resume data must come from disk.
    TransferManager receiver, sender;
    receiver.setReceiveDirectory(destination);
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    qint64 firstProgress = -1;
    connect(&sender, &TransferManager::taskProgress, &sender,
        [&](const QString &, qint64 done, qint64, double) { if (firstProgress < 0) firstProgress = done; });
    QSignalSpy results(&sender, &TransferManager::taskFinished);
    const auto id = sender.restore(source, "127.0.0.1", receiver.port());
    QVERIFY(sender.resume(id));
    QTRY_VERIFY_WITH_TIMEOUT(finished(results, id, true), 10000);
    QCOMPARE(firstProgress, partialLength);
    QCOMPARE(readFile(QDir(destination).filePath("resume.bin")), payload);
}

void Integration::corruptPartialRestarts()
{
    QTemporaryDir temporary;
    const auto payload = sampleData(512 * 1024);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    const auto source = temporary.filePath("corrupt.bin");
    const auto destination = temporary.filePath("received");
    const auto parts = QDir(destination).filePath(".byteme-partials");
    QVERIFY(QDir().mkpath(parts));
    const auto key = Wire::storageKey("corrupt.bin", payload.size(), digest);
    QVERIFY(writeFile(source, payload));
    QVERIFY(writeFile(QDir(parts).filePath(key + ".part"), QByteArray(65536, 'x')));
    QVERIFY(writeFile(QDir(parts).filePath(key + ".json"), QJsonDocument(QJsonObject{{"version", 1},
        {"name", "corrupt.bin"}, {"size", QString::number(payload.size())}, {"sha256", digest}}).toJson()));
    TransferManager receiver, sender;
    receiver.setReceiveDirectory(destination);
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    bool restarted = false;
    connect(&sender, &TransferManager::taskUpdated, &sender,
        [&](const TaskInfo &task) { if (task.state.contains(QStringLiteral("片段不一致"))) restarted = true; });
    QSignalSpy result(&sender, &TransferManager::taskFinished);
    const auto id = sender.sendFile(source, "127.0.0.1", receiver.port());
    QTRY_VERIFY_WITH_TIMEOUT(finished(result, id, true), 10000);
    QVERIFY(restarted);
    QCOMPARE(readFile(QDir(destination).filePath("corrupt.bin")), payload);
}

void Integration::rejectOffer()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("rejected.bin");
    QVERIFY(writeFile(path, "test"));
    TransferManager receiver, sender;
    receiver.setReceiveDirectory(temporary.filePath("received"));
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    connect(&receiver, &TransferManager::incomingOffer, &receiver,
        [&](const QString &id, const QString &, const QString &, qint64) { receiver.approve(id, false); });
    QSignalSpy results(&sender, &TransferManager::taskFinished);
    QSignalSpy rejected(&receiver, &TransferManager::taskFinished);
    const auto id = sender.sendFile(path, "127.0.0.1", receiver.port());
    QTRY_VERIFY_WITH_TIMEOUT(finished(results, id, false), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!rejected.isEmpty(), 3000);
    QVERIFY(rejected.last().at(2).toString().contains(QStringLiteral("拒绝")));
    QVERIFY(!QFileInfo::exists(temporary.filePath("received/rejected.bin")));
}

void Integration::invalidOffer_data()
{
    QTest::addColumn<QJsonObject>("object");
    QTest::newRow("traversal") << offer("../escaped.bin", 1, QString(64, 'a'));
    QTest::newRow("reserved") << offer("CON.txt", 1, QString(64, 'a'));
    QTest::newRow("digest") << offer("file.bin", 1, QString(64, 'z'));
    QTest::newRow("too-large") << offer("file.bin", Wire::MaxFileSize + 1, QString(64, 'a'));
    auto invalid = offer("file.bin", 1, QString(64, 'a'));
    invalid["version"] = 2;
    QTest::newRow("version") << invalid;
}

void Integration::invalidOffer()
{
    QFETCH(QJsonObject, object);
    QTemporaryDir temporary;
    TransferManager receiver;
    receiver.setReceiveDirectory(temporary.filePath("received"));
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy result(&receiver, &TransferManager::taskFinished);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, receiver.port());
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, 3000);
    const auto bytes = Wire::control(object);
    // Fragment the length header to exercise framing on a real TCP stream.
    socket.write(bytes.left(2));
    QTest::qWait(20);
    socket.write(bytes.mid(2));
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 3000);
    QVERIFY(!result.last().at(1).toBool());
    QVERIFY(!QFileInfo::exists(temporary.filePath("escaped.bin")));
    QVERIFY(!QFileInfo::exists(temporary.filePath("received")));
}

void Integration::oversizeFrame()
{
    TransferManager receiver;
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy result(&receiver, &TransferManager::taskFinished);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, receiver.port());
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, 3000);
    QByteArray header(4, '\0');
    qToBigEndian<quint32>(0xffffffff, header.data());
    socket.write(header);
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 3000);
    QVERIFY(!result.last().at(1).toBool());
}

void Integration::incompleteTransferIsNeverCommitted()
{
    QTemporaryDir temporary;
    TransferManager receiver;
    receiver.setReceiveDirectory(temporary.filePath("received"));
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy result(&receiver, &TransferManager::taskFinished);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, receiver.port());
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, 3000);
    socket.write(Wire::control(offer("truncated.bin", 100, QString(64, 'a'))));
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 3000);
    socket.readAll();
    socket.write(Wire::control({{"type", "begin"}, {"offset", "0"}})
                 + Wire::pack(Wire::Kind::Data, "only five") + Wire::control({{"type", "finish"}}));
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 3000);
    QVERIFY(!result.last().at(1).toBool());
    QVERIFY(!QFileInfo::exists(temporary.filePath("received/truncated.bin")));
    QCOMPARE(QDir(temporary.filePath("received/.byteme-partials")).entryList({"*.part"}, QDir::Files).size(), 1);
}

void Integration::checksumMismatchIsNeverCommitted()
{
    QTemporaryDir temporary;
    TransferManager receiver;
    receiver.setReceiveDirectory(temporary.filePath("received"));
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    QSignalSpy result(&receiver, &TransferManager::taskFinished);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, receiver.port());
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, 3000);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(QByteArray(100, 'a'), QCryptographicHash::Sha256).toHex());
    socket.write(Wire::control(offer("bad.bin", 100, digest)));
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 3000);
    socket.readAll();
    socket.write(Wire::control({{"type", "begin"}, {"offset", "0"}})
        + Wire::pack(Wire::Kind::Data, QByteArray(100, 'b')) + Wire::control({{"type", "finish"}}));
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 3000);
    QVERIFY(!result.last().at(1).toBool());
    QVERIFY(result.last().at(2).toString().contains(QStringLiteral("SHA-256 校验失败")));
    QVERIFY(!QFileInfo::exists(temporary.filePath("received/bad.bin")));
}

void Integration::shutdownDuringTransfer()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("shutdown.bin");
    QVERIFY(writeFile(path, sampleData(4 * 1024 * 1024)));
    TransferManager receiver;
    receiver.setReceiveDirectory(temporary.filePath("received"));
    receiver.setAutoAccept(true);
    QVERIFY(receiver.listen(0, QHostAddress::LocalHost));
    auto sender = std::make_unique<TransferManager>();
    bool started = false;
    connect(sender.get(), &TransferManager::taskProgress, &receiver,
        [&](const QString &, qint64, qint64, double) { started = true; });
    sender->sendFile(path, "127.0.0.1", receiver.port(), 10);
    QTRY_VERIFY_WITH_TIMEOUT(started, 5000);
    sender.reset(); // Must stop and join the worker without a queued-quit deadlock.
    receiver.shutdown();
}

QTEST_GUILESS_MAIN(Integration)
#include "integration.moc"
