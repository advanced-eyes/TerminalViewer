#pragma once

namespace TestFonts
{
// 테스트용 Cascadia Mono(Regular, Bold, Italic, Bold Italic)를 글꼴 데이터베이스에 등록한다. 여러 번 불러도 한 번만 등록된다.
// 글꼴은 테스트 실행 파일에만 들어간다(tests/fonts/TestFonts.qrc). 라이브러리는 글꼴을 갖지 않는다: 쓰는 쪽(Ocean)이 넣는다.
void Register();
}
