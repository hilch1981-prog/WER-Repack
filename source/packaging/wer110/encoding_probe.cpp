#include "Util.h"
#include <string>

int main()
{
    utf8printf(stdout, "%s\n", "한국 에뮬레이터 연구소 WER VER1.1.0");
    std::string longLine;
    for (int i = 0; i < 40000; ++i)
        longLine += "한";
    utf8printf(stdout, "%s\n", longLine.c_str());
    utf8printf(stderr, "%s %d\n", "한국어 오류 출력", 12340);
    return 0;
}
