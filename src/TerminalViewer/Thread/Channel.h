#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace TerminalViewer
{
// 스레드 사이 명령 큐. Stop 뒤에도 남은 값은 꺼낼 수 있고, 비면 Pop 이 false 를 낸다.
// 이름 공간에 넣는 까닭: 쓰는 쪽(Ocean)도 같은 이름의 전역 템플릿을 갖는다(재정의·ODR 위반을 피한다).
template <typename T>
class Channel
{
public:
    void Push(T value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_deqValues.push_back(std::move(value));
        }
        m_cv.notify_one();
    }

    bool Pop(T& out)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait(lock, [this] { return !m_deqValues.empty() || m_bStopped; });
        return TakeLocked(out);
    }

    bool PopUntil(T& out, std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait_until(lock, deadline, [this] { return !m_deqValues.empty() || m_bStopped; });
        return TakeLocked(out);
    }

    bool TryPop(T& out)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return TakeLocked(out);
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_bStopped = true;
        }
        m_cv.notify_all();
    }

    void Restart()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_bStopped = false;
    }

private:
    bool TakeLocked(T& out)
    {
        if (m_deqValues.empty())
        {
            return false;
        }
        out = std::move(m_deqValues.front());
        m_deqValues.pop_front();
        return true;
    }

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<T> m_deqValues;
    bool m_bStopped = false;
};
}
