//
// Created by 陈志帅 on 2020/4/15.
//

#include <string>
using namespace std;


extern void playAudio(const string& inputPath);

extern void playVideo(const string& inputPath);

int main() {

    string inputPath = "/Users/chenzhishuai/Downloads/baidunetdiskdownload/情书.rmvb";
    playAudio(inputPath);
    playVideo(inputPath);
    return 0;
};



