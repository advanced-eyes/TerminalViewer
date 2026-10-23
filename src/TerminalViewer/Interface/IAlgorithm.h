#pragma once

namespace TerminalViewer
{
// 전략을 바꿔 끼울 수 있는 알고리즘의 공통 인터페이스.
// 이름 공간에 넣는 까닭: 쓰는 쪽(Ocean)도 같은 이름의 전역 템플릿을 갖는다. 한 번역 단위가 둘을 함께 include 해도
// 재정의가 되지 않고, 따로 include 해도 정의가 갈라져 ODR 을 어기지 않게 한다.
template <typename TInput, typename TOutput, typename TStrategyType>
class IAlgorithm
{
public:
    virtual ~IAlgorithm() = default;

    virtual void SetStrategy(TStrategyType eType) = 0;
    virtual bool Execute(const TInput& in, TOutput& out) = 0;
};
}
