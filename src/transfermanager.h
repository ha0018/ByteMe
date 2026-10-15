#pragma once
#include "transferworker.h"
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QTcpServer>
#include <QThread>

struct TaskInfo {
    QString id;
    QString name;
    QString peer;
    QString source;
    QString host;
    quint16 port = 0;
    qint64 size = 0;
    bool outgoing = false;
    bool active = false;
    bool success = false;
    QString state;
    QString savedPath;
};
Q_DECLARE_METATYPE(TaskInfo)

class DescriptorServer final : public QTcpServer {
    Q_OBJECT
public:
    using QTcpServer::QTcpServer;
signals:
    void accepted(qintptr descriptor);
protected:
    void incomingConnection(qintptr descriptor) override { emit accepted(descriptor); }
};

class TransferManager final : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager() override;
    bool listen(quint16 port = 45455, const QHostAddress &address = QHostAddress::AnyIPv4);
    quint16 port() const { return m_server.serverPort(); }
    void setReceiveDirectory(const QString &path) { m_receiveDirectory = path; }
    void setAutoAccept(bool enabled) { m_autoAccept = enabled; }
    QString sendFile(const QString &path, const QString &host, quint16 port, int chunkDelayMs = 0);
    QString restore(const QString &path, const QString &host, quint16 port);
    bool resume(const QString &id);
    void pause(const QString &id);
    void approve(const QString &id, bool accepted);
    void shutdown();
    QList<TaskInfo> tasks() const { return m_tasks.values(); }
signals:
    void taskAdded(const TaskInfo &task);
    void taskUpdated(const TaskInfo &task);
    void taskProgress(const QString &id, qint64 done, qint64 total, double speed);
    void incomingOffer(const QString &id, const QString &name, const QString &peer, qint64 size);
    void taskFinished(const QString &id, bool success, const QString &message, const QString &savedPath);
    void error(const QString &message);
private:
    struct Active { QThread *thread = nullptr; QPointer<TransferWorker> worker; };
    void receive(qintptr descriptor);
    void launch(TransferWorker *worker, const QString &id);
    void launchQueued();
    DescriptorServer m_server;
    QHash<QString, TaskInfo> m_tasks;
    QHash<QString, Active> m_active;
    QQueue<SendRequest> m_queue;
    QHash<QString, SendRequest> m_requests;
    QString m_receiveDirectory;
    bool m_autoAccept = false;
    bool m_stopping = false;
};
