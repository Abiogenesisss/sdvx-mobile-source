#pragma once

#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sdvx {
// Integer units are 0.001 VOLFORCE. The profile sums the best 50 chart records.
inline constexpr std::array ForceThresholds{
    0,2500,5000,7500,10000,10500,11000,11500,12000,12500,13000,13500,
    14000,14250,14500,14750,15000,15250,15500,15750,16000,16250,16500,16750,
    17000,17250,17500,17750,18000,18250,18500,18750,19000,19250,19500,19750,
    20000,21000,22000,23000}; // NABLA 180964060
inline int forceBand(int force) {
    return int(std::upper_bound(ForceThresholds.begin(), ForceThresholds.end(), force) - ForceThresholds.begin()) - 1;
}
inline int forceEffect(int before, int after) {
    if (after <= before) return 0;
    const int beforeBand = forceBand(before), afterBand = forceBand(after);
    return afterBand / 4 > beforeBand / 4 ? 3 : afterBand > beforeBand ? 2 : 1;
}
inline int resultGrade(std::string_view grade) {
    constexpr std::array<std::string_view,10> names{"d","c","b","a","a_plus","aa","aa_plus","aaa","aaa_plus","s"};
    return int(std::find(names.begin(),names.end(),grade)-names.begin())+1;
}
inline int chartForce(int levelTenths, const Result& result) {
    constexpr std::array coefficients{0,80,82,85,88,91,94,97,100,102,105};
    const int grade = std::min(resultGrade(result.grade),10);
    const int lamp = result.outcome == "crash" ? 50 : result.outcome == "uc" ? 106
        : result.outcome == "puc" || result.outcome == "spuc" ? 110 : result.excessive ? 102 : 100;
    return int(int64_t(levelTenths) * result.score * coefficients[grade] * lamp / 50000000000LL);
}
struct StageBlockAward {
    int stage = 0, paseli = 0, clear = 0;
    int total() const { return stage + paseli + clear; }
};

inline StageBlockAward stageBlocks(int levelTenths, const Result& result, bool paidWithPaseli = false,
                                  int capacity = 99999999) {
    // 1805f6f50 -> 1805f9fd0 computes the stage amount. 1805f8d10 applies
    // the PASELI percentage to that amount before the clear bonuses.
    const int level = std::clamp(levelTenths / 10,1,20);
    const float a=(level-1)*.0714285746216774f;
    const float b=(std::clamp(resultGrade(result.grade),1,11)-1)*.1111111119389534f;
    const float gauge=std::clamp(result.gauge,0.f,1.f), c=gauge*gauge;
    const float value=(c+1)*std::sqrt(a*b)*b*.3374999761581421f
        +(c+1)*std::sqrt(b)*b*.11249999701976776f
        +(c+1)*std::sqrt(a)*.03750000149011612f+c*.012500000186264515f+.012500000186264515f;
    const int stage=50+int(std::ceil(std::clamp(value,0.f,1.2000000476837158f)*30));
    int clear=0;
    if (result.outcome=="uc") clear+=int(std::ceil(level*5*.3333333432674408f));
    if (result.outcome=="puc" || result.outcome=="spuc") clear+=int(std::ceil(level*5*.5f));
    if (!result.excessive && result.outcome!="crash" && int(gauge*100)==70) clear+=10;
    StageBlockAward award;
    award.stage = std::min(stage, capacity);
    award.paseli = paidWithPaseli ? std::min(stage / 10, capacity - award.stage) : 0;
    award.clear = std::min(clear, capacity - award.stage - award.paseli);
    return award;
}
}
