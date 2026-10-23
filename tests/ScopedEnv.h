#pragma once

#include <QByteArray>
#include <QtGlobal>

// 환경 변수를 잠시 바꾸고, 범위를 벗어날 때(테스트가 중간에 실패해 돌아와도) 원래대로 되돌린다.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const QByteArray& value)
        : m_pName(name)
        , m_bHad(qEnvironmentVariableIsSet(name))
        , m_oldValue(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~ScopedEnv()
    {
        if (m_bHad)
            qputenv(m_pName, m_oldValue);
        else
            qunsetenv(m_pName);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_pName;
    bool m_bHad;
    QByteArray m_oldValue;
};
