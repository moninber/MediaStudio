#pragma once

#include <QTemporaryDir>
#include <filesystem>

// 生成带索引、非静音且时长一致的本地三类媒体，回归不依赖用户素材或外部编码器。
// 每次测试独占临时目录，析构自动释放；可用环境变量将本轮数据放到项目外验证目录。
class LocalMediaFixtures {
public:
    LocalMediaFixtures();
    std::filesystem::path audioVideo() const;
    std::filesystem::path audioOnly() const;
    std::filesystem::path videoOnly() const;

private:
    QTemporaryDir directory_;
};
