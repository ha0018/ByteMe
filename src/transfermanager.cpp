#include "transfermanager.h"
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

TransferManager::TransferManager(QObject *parent) : QObject(parent), m_server(this)
{
    qRegisterMetaType<TaskInfo>();
    m_receiveDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/ByteMe";
    connect(&m_server, &DescriptorServer::accepted, this, &TransferManager::receive);
}

TransferManager::~TransferManager()
{
    shutdown();
}

bool TransferManager::listen(quint16 port, const QHostAddress &address)
{
    m_server.close();
    if (!m_server.listen(address, port)) {
        emit error(QStringLiteral("TCP 服务启动失败：%1").arg(m_server.errorString()));
        return false;
    }
    return true;
}

QString TransferManager::restore(const QString &path, const QString &host, quint16 port)
{
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    TaskInfo task;
    task.id = id;
    task.name = QFileInfo(path).fileName();
    task.peer = QStringLiteral("%1:%2").arg(host).arg(port);
    task.source = QFileInfo(path).absoluteFilePath();
    task.host = host;
    task.port = port;
    task.size = QFileInfo(path).size();
    task.outgoing = true;
    task.state = QStringLiteral("已暂停，可继续");
    m_tasks.insert(id, task);
    m_requests.insert(id, {id, task.source, host, port, 0});
    emit taskAdded(task);
    return id;
}

QString TransferManager::sendFile(const QString &path, const QString &host, quint16 port, int chunkDelayMs)
{
    if (m_stopping || host.trimmed().isEmpty() || port == 0)
        return {};
    const auto id = restore(path, host.trimmed(), port);
    m_requests[id].chunkDelayMs = qBound(0, chunkDelayMs, 1000);
    resume(id);
    return id;
}

bool TransferManager::resume(const QString &id)
{
    if (m_stopping || !m_tasks.contains(id) || !m_tasks[id].outgoing || m_tasks[id].success)
        return false;
    if (m_active.contains(id) || m_tasks[id].active)
        return false;
    auto &task = m_tasks[id];
    task.active = true;
    task.state = QStringLiteral("排队中");
    emit taskUpdated(task);
    m_queue.enqueue(m_requests.value(id));
    launchQueued();
    return true;
}

void TransferManager::launchQueued()
{
    if (m_stopping)
        return;
    int outgoing = 0;
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it)
        if (m_tasks.value(it.key()).outgoing) ++outgoing;
    while (!m_queue.isEmpty() && outgoing < 4 && m_active.size() < 8) {
        const auto request = m_queue.dequeue();
        launch(new TransferWorker(request), request.id);
        ++outgoing;
    }
}

void TransferManager::receive(qintptr descriptor)
{
    if (m_stopping || m_active.size() >= 8) {
        QTcpSocket rejected;
        rejected.setSocketDescriptor(descriptor);
        rejected.abort();
        return;
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    TaskInfo task;
    task.id = id;
    task.name = QStringLiteral("待接收文件");
    task.active = true;
    task.state = QStringLiteral("等待文件信息");
    m_tasks.insert(id, task);
    emit taskAdded(task);
    launch(new TransferWorker(id, descriptor, m_receiveDirectory, m_autoAccept), id);
}

void TransferManager::launch(TransferWorker *worker, const QString &id)
{
    auto *thread = new QThread(this);
    thread->setObjectName(QStringLiteral("transfer-%1").arg(id));
    worker->moveToThread(thread);
    m_active.insert(id, {thread, worker});
    connect(thread, &QThread::started, worker, &TransferWorker::start);
    connect(worker, &TransferWorker::stateChanged, this, [this](const QString &taskId, const QString &state) {
        auto &task = m_tasks[taskId];
        task.state = state;
        emit taskUpdated(task);
    });
    connect(worker, &TransferWorker::metadata, this,
            [this](const QString &taskId, const QString &name, const QString &peer, qint64 size) {
        auto &task = m_tasks[taskId];
        task.name = name;
        task.peer = peer;
        task.size = size;
        emit taskUpdated(task);
    });
    connect(worker, &TransferWorker::progress, this, &TransferManager::taskProgress);
    connect(worker, &TransferWorker::offered, this, &TransferManager::incomingOffer);
    connect(worker, &TransferWorker::finished, this,
            [this](const QString &taskId, bool success, const QString &message, const QString &path) {
        auto &task = m_tasks[taskId];
        task.active = false;
        task.success = success;
        task.state = message;
        task.savedPath = path;
        emit taskUpdated(task);
        emit taskFinished(taskId, success, message, path);
    });
    connect(worker, &TransferWorker::finished, worker, &QObject::deleteLater);
    // DirectConnection is required: shutdown waits in the GUI thread, so a queued quit would deadlock.
    connect(worker, &TransferWorker::finished, thread, &QThread::quit, Qt::DirectConnection);
    connect(thread, &QThread::finished, this, [this, thread, id] {
        m_active.remove(id);
        thread->deleteLater();
        launchQueued();
    });
    thread->start();
}

void TransferManager::pause(const QString &id)
{
    for (int index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).id == id) {
            m_queue.removeAt(index);
            auto &task = m_tasks[id];
            task.active = false;
            task.state = QStringLiteral("已暂停，可继续");
            emit taskUpdated(task);
            return;
        }
    }
    const auto active = m_active.value(id);
    if (active.worker)
        QMetaObject::invokeMethod(active.worker, &TransferWorker::stop, Qt::QueuedConnection);
}

void TransferManager::approve(const QString &id, bool accepted)
{
    const auto worker = m_active.value(id).worker;
    if (worker)
        QMetaObject::invokeMethod(worker, [worker, accepted] { if (worker) worker->approve(accepted); }, Qt::QueuedConnection);
}

void TransferManager::shutdown()
{
    if (m_stopping)
        return;
    m_stopping = true;
    m_server.close();
    m_queue.clear();
    const auto activeTasks = m_active.values();
    for (const auto &active : activeTasks)
        if (active.worker)
            QMetaObject::invokeMethod(active.worker, &TransferWorker::stop, Qt::QueuedConnection);
    for (const auto &active : activeTasks)
        active.thread->wait();
    m_active.clear();
}
