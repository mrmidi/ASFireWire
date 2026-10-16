// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexHardwareRender.hpp"
#include <complex>
#include <vector>
namespace Pro40Lab {
// Diagnostic content only: output6 -> external cable -> LINE input7. The
// selected input is retained in owned RAM until analysis AFTER owner joins.
// No microphone channels are retained and no PCM is written to disk. No
// routing, clock, packet framing or native scheduling changes.
struct DuplexLoopbackContent {
    static constexpr unsigned seconds=24,rate=48000,chipFrames=8,chipCount=1023;
    static constexpr unsigned markerFrames=chipFrames*chipCount,markers=5;
    static constexpr float amplitude=0.0316227766f; // -30 dBFS peak.
    static constexpr uint64_t markerStart(unsigned m){return uint64_t(2+4*m)*rate;}
    std::array<float,markerFrames> reference{};
    // Include the final partial callback. Allocation is before audio starts.
    std::vector<float> captured=std::vector<float>(((seconds*rate+136)/137)*137);
    DuplexLoopbackContent() {
        // Deterministic non-periodic marker inside each burst. A fixed xorshift
        // seed avoids dependence on a platform random generator or audio input.
        uint32_t state=0x934d62e1;
        for(unsigned chip=0;chip<chipCount;++chip) {
            state^=state<<13;state^=state>>17;state^=state<<5;
            for(unsigned j=0;j<chipFrames;++j) {
                const auto f=chip*chipFrames+j;
                const double fade=std::min({1.,double(f)/240,double(markerFrames-1-f)/240});
                reference[f]=float((state&1?amplitude:-amplitude)*fade);
            }
        }
    }
    void tones(uint64_t first,std::span<float> out) const noexcept {
        std::fill(out.begin(),out.end(),0.f);
        for(unsigned m=0;m<markers;++m)for(size_t f=0;f<out.size()/6;++f) {
            const auto at=first+f;
            if(at>=markerStart(m) && at-markerStart(m)<markerFrames)
                out[f*6+5]=reference[at-markerStart(m)];
        }
    }
    void observe(uint64_t first,std::span<const float> input) noexcept {
        for(size_t f=0;f<input.size()/8 && first+f<captured.size();++f)captured[first+f]=input[f*8+6];
    }
};
using DuplexLoopbackRender=BasicDuplexHardwareRender<DuplexLoopbackContent>;
static_assert(DuplexLoopbackRender::blocks==8409);

// Offline bounded correlation. Never call from audio/native callbacks or while
// capture storage has a writer. FFT is only an analysis implementation, not a
// change to the emitted PCM. Test against direct dot products and delayed,
// filtered, attenuated, noisy/inverted, clipped, missing and ambiguous returns.
namespace LoopbackAnalysis {
using Complex=std::complex<double>;
inline void fft(std::vector<Complex> &v,bool inverse) {
    const auto n=v.size();
    for(size_t i=1,j=0;i<n;++i) {
        size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;
        if(i<j)std::swap(v[i],v[j]);
    }
    constexpr double tau=6.283185307179586476925286766559;
    for(size_t length=2;length<=n;length<<=1) {
        const auto step=std::polar(1.,(inverse?tau:-tau)/length);
        for(size_t i=0;i<n;i+=length) {
            Complex phase=1;
            for(size_t j=0;j<length/2;++j) {
                const auto a=v[i+j],b=v[i+j+length/2]*phase;
                v[i+j]=a+b;v[i+j+length/2]=a-b;phase*=step;
            }
        }
    }
    if(inverse)for(auto &x:v)x/=n;
}
struct Match {
    bool valid=false;
    int64_t inputMinusOutput=0,callbackReadback=0;
    uint64_t recordedIndex=0;
    double correlation=0,competitor=0,peak=0,rms=0;
};
inline Match find(std::span<const float> input,std::span<const float> reference,
                  size_t begin,size_t end) {
    Match result;
    if(reference.empty() || begin>=end || end>input.size() || reference.size()>input.size()-end+1)return result;
    // begin/end are candidate start indices, end exclusive. The entire marker
    // must be present at every candidate. Hard cap keeps malformed requests
    // from turning bounded diagnostics into unbounded allocations.
    const size_t count=end-begin,signalSize=count+reference.size()-1;
    if(count>96001 || reference.size()>16384)return result;
    size_t n=1;while(n<signalSize+reference.size()-1)n<<=1;
    std::vector<Complex> signal(n),pattern(n);
    std::vector<double> sums(signalSize+1),squares(signalSize+1),score(count);
    double refSum=0,refSquares=0;
    for(size_t i=0;i<signalSize;++i) {
        const double x=input[begin+i];if(!std::isfinite(x))return result;
        signal[i]=x;sums[i+1]=sums[i]+x;squares[i+1]=squares[i]+x*x;
    }
    for(size_t i=0;i<reference.size();++i) {
        const double x=reference[i];if(!std::isfinite(x))return result;
        pattern[reference.size()-1-i]=x;refSum+=x;refSquares+=x*x;
    }
    const double refEnergy=refSquares-refSum*refSum/reference.size();if(refEnergy<=1e-12)return result;
    fft(signal,false);fft(pattern,false);
    for(size_t i=0;i<n;++i)signal[i]*=pattern[i];fft(signal,true);
    size_t best=0;
    for(size_t i=0;i<count;++i) {
        const double sum=sums[i+reference.size()]-sums[i];
        const double energy=squares[i+reference.size()]-squares[i]-sum*sum/reference.size();
        if(energy>1e-12)score[i]=(signal[i+reference.size()-1].real()-sum*refSum/reference.size())/std::sqrt(energy*refEnergy);
        if(std::abs(score[i])>std::abs(score[best]))best=i;
    }
    result.recordedIndex=begin+best;result.correlation=score[best];
    for(size_t i=0;i<count;++i)if(i+16<best || i>best+16)result.competitor=std::max(result.competitor,std::abs(score[i]));
    double energy=0,sum=0;
    for(size_t i=0;i<reference.size();++i){const double x=input[result.recordedIndex+i];energy+=x*x;sum+=x;result.peak=std::max(result.peak,std::abs(x));}
    result.rms=std::sqrt(std::max(0.,energy/reference.size()-std::pow(sum/reference.size(),2))); // AC RMS, ignore DC bias.
    result.valid=best>0 && best+1<count && std::abs(result.correlation)>=.70 &&
        result.competitor<std::abs(result.correlation)*.75 && result.rms>=1e-4 && result.peak<.98;
    return result;
}
struct Report {
    std::array<Match,DuplexLoopbackContent::markers> matches{};
    bool valid=false;
    int64_t minimum=0,maximum=0;
};
inline Report analyze(const DuplexLoopbackContent &content,uint64_t inputFirst,uint64_t outputFirstSample) {
    Report report;
    if(inputFirst>Pro40Audio::SourceFrameClock::maxExactFrame || outputFirstSample>Pro40Audio::SourceFrameClock::maxExactFrame)return report;
    bool all=true;
    for(unsigned m=0;m<content.markers;++m) {
        const auto marker=content.markerStart(m);
        const int64_t predicted=int64_t(outputFirstSample)+int64_t(marker)-int64_t(inputFirst);
        const int64_t begin=predicted-48000,end=predicted+48001;
        if(begin<0 || end<0){all=false;continue;}
        auto &match=report.matches[m];match=find(content.captured,content.reference,size_t(begin),size_t(end));
        match.inputMinusOutput=int64_t(inputFirst)+int64_t(match.recordedIndex)-int64_t(outputFirstSample)-int64_t(marker);
        match.callbackReadback=int64_t(match.recordedIndex)-int64_t(marker);
        all=all && match.valid;
    }
    report.minimum=report.maximum=report.matches[0].inputMinusOutput;
    for(const auto &m:report.matches){report.minimum=std::min(report.minimum,m.inputMinusOutput);report.maximum=std::max(report.maximum,m.inputMinusOutput);}
    report.valid=all && report.maximum-report.minimum<=2;
    return report;
}
} // namespace LoopbackAnalysis
} // namespace Pro40Lab
