#pragma once
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <memory>

struct SendRequest {
    QString id;
    QString path;
    QString host;
    quint16 port = 45455;
    int chunkDelayMs = 0; // Also allows deterministic interruption tests.
};

class TransferWorker final : public QObject {
    Q_OBJECT
public:
    explicit TransferWorker(const SendRequest &request);
    TransferWorker(const QString &id, qintptr descriptor, const QString &receiveDirectory, bool autoAccept);
    ~TransferWorker() override;
public slots:
    void start();
    void stop();
    void approve(bool accepted);
signals:
    void stateChanged(const QString &id, const QString &state);
    void metadata(const QString &id, const QString &name, const QString &peer, qint64 size);
    void progress(const QString &id, qint64 done, qint64 total, double bytesPerSecond);
    void offered(const QString &id, const QString &name, const QString &peer, qint64 size);
    void finished(const QString &id, bool success, const QString &message, const QString &savedPath);
private:
    enum class State { Idle, HashingSource, Connecting, AwaitOffer, AwaitApproval, HashingPrefix,
                       AwaitReady, AwaitBegin, Sending, Receiving, AwaitComplete, Verifying, Done };
    void createSocket();
    void readFrames();
    void handleControl(const QJsonObject &object);
    void handleData(const QByteArray &data);
    bool sendControl(const QJsonObject &object);
    void acceptOffer(const QJsonObject &object);
    void prepareReceive();
    void sendReady();
    void prepareSend();
    void pump();
    void verifyAndCommit();
    void commitReceived();
    void hashFile(const QString &path, qint64 length, std::function<void(const QString &)> callback);
    void hashStep();
    bool sourceUnchanged() const;
    void publishProgress(bool force = false);
    void finish(bool success, const QString &message, const QString &path = {});
    void fail(const QString &message, bool notifyPeer = true);

    const QString m_id;
    const bool m_sender;
    SendRequest m_request;
    qintptr m_descriptor = -1;
    QString m_receiveDirectory;
    bool m_autoAccept = false;
    State m_state = State::Idle;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_watchdog = nullptr;
    QTimer *m_keepAlive = nullptr;
    QByteArray m_buffer;
    QFile m_file;
    QFile m_hashFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_hashRemaining = 0;
    std::function<void(const QString &)> m_hashCallback;
    QString m_name;
    QString m_digest;
    QString m_prefixDigest;
    QString m_partPath;
    QString m_metaPath;
    QString m_peer;
    qint64 m_size = 0;
    qint64 m_position = 0;
    qint64 m_resumeOffset = 0;
    qint64 m_speedBaseline = 0;
    QDateTime m_sourceModified;
    QElapsedTimer m_speedClock;
    QElapsedTimer m_progressClock;
    std::unique_ptr<QLockFile> m_lock;
    bool m_pumpScheduled = false;
    bool m_readScheduled = false;
};
