#pragma once
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

struct Peer {
    QString id;
    QString name;
    QHostAddress address;
    quint16 port = 0;
    QDateTime lastSeen;
};
Q_DECLARE_METATYPE(Peer)

class Discovery final : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject *parent = nullptr);
    bool start(const QString &id, const QString &name, quint16 tcpPort, quint16 udpPort = 45454);
    void stop();
    void probe(const QHostAddress &address = QHostAddress::Broadcast);
signals:
    void peerSeen(const Peer &peer);
    void peerGone(const QString &id);
    void error(const QString &message);
private:
    void receive();
    void announce(const QString &type, const QHostAddress &address, quint16 destinationPort);
    void heartbeat();
    QUdpSocket m_socket;
    QTimer m_timer;
    QHash<QString, Peer> m_peers;
    QString m_id;
    QString m_name;
    quint16 m_tcpPort = 0;
    quint16 m_udpPort = 45454;
};
