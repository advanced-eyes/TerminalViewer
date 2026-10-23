#include "TerminalViewer/Thread/Channel.h"

#include <QTest>
#include <chrono>
#include <future>
#include <thread>

class ChannelTest : public QObject
{
    Q_OBJECT

private slots:
    void keepsOrder()
    {
        TerminalViewer::Channel<int> channel;
        channel.Push(1);
        channel.Push(2);
        channel.Push(3);
        int iValue = 0;
        QVERIFY(channel.Pop(iValue));
        QCOMPARE(iValue, 1);
        QVERIFY(channel.TryPop(iValue));
        QCOMPARE(iValue, 2);
        QVERIFY(channel.Pop(iValue));
        QCOMPARE(iValue, 3);
        QVERIFY(!channel.TryPop(iValue));
    }

    void popUntilTimesOut()
    {
        TerminalViewer::Channel<int> channel;
        int iValue = 0;
        const auto start = std::chrono::steady_clock::now();
        QVERIFY(!channel.PopUntil(iValue, start + std::chrono::milliseconds(50)));
        const auto llElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        QVERIFY2(llElapsedMs >= 45 && llElapsedMs <= 500, qPrintable(QString::number(llElapsedMs)));
    }

    void stopWakesBlockedPop()
    {
        TerminalViewer::Channel<int> channel;
        auto result = std::async(std::launch::async, [&channel] { int iValue = 0; return channel.Pop(iValue); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QVERIFY(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        channel.Stop();
        QVERIFY(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(!result.get());
    }

    void remainingValuesSurviveStopAndRestartBlocksAgain()
    {
        TerminalViewer::Channel<int> channel;
        channel.Push(7);
        channel.Stop();
        int iValue = 0;
        QVERIFY(channel.Pop(iValue));
        QCOMPARE(iValue, 7);
        QVERIFY(!channel.Pop(iValue));
        channel.Restart();
        QVERIFY(!channel.PopUntil(iValue, std::chrono::steady_clock::now() + std::chrono::milliseconds(30)));
        auto result = std::async(std::launch::async, [&channel] { int iValue = 0; return channel.Pop(iValue); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QVERIFY(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        channel.Stop();
        QVERIFY(!result.get());
    }

    void stopWakesBlockedPopUntilWithFarDeadline()
    {
        TerminalViewer::Channel<int> channel;
        auto result = std::async(std::launch::async,
                                 [&channel]
                                 {
                                     int iValue = 0;
                                     return channel.PopUntil(iValue, std::chrono::steady_clock::now() + std::chrono::seconds(5));
                                 });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto begin = std::chrono::steady_clock::now();
        channel.Stop();
        QVERIFY(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        QVERIFY(!result.get());
        QVERIFY(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    }

    void pushFromOtherThreadWakesBlockedPop()
    {
        TerminalViewer::Channel<int> channel;
        auto result = std::async(std::launch::async,
                                 [&channel]
                                 {
                                     int iValue = 0;
                                     const bool bOk = channel.Pop(iValue);
                                     return bOk ? iValue : -1;
                                 });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QVERIFY(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        std::thread pusher([&channel] { channel.Push(42); });
        QVERIFY(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QCOMPARE(result.get(), 42);
        pusher.join();
    }
};

QObject* CreateChannelTest() { return new ChannelTest(); }

#include "ChannelTest.moc"
