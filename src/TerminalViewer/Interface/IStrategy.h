#pragma once

namespace TerminalViewer
{
// 특정 컨셉을 지닌 알고리즘 구현체의 공통 인터페이스.
// 같은 알고리즘의 CPU/GPU 구현은 *CPU / *GPU 로 이름을 나눠 각각 이 인터페이스를 구현한다.
// 이름 공간에 넣는 까닭은 IAlgorithm.h 와 같다(쓰는 쪽의 같은 이름 템플릿과 겹치지 않게).
template <typename TInput, typename TOutput, typename TStrategyType>
class IStrategy
{
public:
    virtual ~IStrategy() = default;

    virtual TStrategyType GetType() const = 0;
    virtual bool Execute(const TInput& in, TOutput& out) = 0;
};
}
