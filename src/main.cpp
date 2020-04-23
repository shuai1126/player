//
// Created by 陈志帅 on 2020/4/15.
//

#include <string>
using namespace std;


extern void playAudio(const string& inputPath);

extern void playVideo(const string& inputPath);

extern void play(const string& inputPath);

int main() {

    string inputPath = "/Users/chenzhishuai/Downloads/baidunetdiskdownload/不能说的秘密.BD1280超清国语中字.mp4";
    play(inputPath);
//    playVideo(inputPath);
    return 0;
};



