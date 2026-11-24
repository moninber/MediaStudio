#include "PacketQueue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Packet {
    AVPacket* value = av_packet_alloc();
    explicit Packet(int bytes = 8) {
        require(value && av_new_packet(value, bytes) >= 0, "could not allocate test packet");
        value->pts = value->dts = 0;
    }
    ~Packet() { av_packet_free(&value); }
    void at(std::int64_t timestamp) { value->pts = value->dts = timestamp; }
};
constexpr PlaybackEpoch Epoch{1, 1, 1};
void queued(PacketQueue& queue, Packet& packet) {
    require(queue.push(packet.value, Epoch).status == PacketPushStatus::Queued, "valid packet rejected");
}

// 分别越过包数、字节和时间跨度，必须保留原包与原统计；出队后同一候选可再次入队。
void testLimits() {
    Packet input, output;
    PlaybackEpoch popped;
    PacketQueue packets;
    packets.reset({2, 0, 0us}, {1, 1000});
    queued(packets, input); queued(packets, input);
    const auto full = packets.push(input.value, Epoch);
    require(full.status == PacketPushStatus::Full && full.limit == PacketQueueLimit::Packets &&
        packets.stats().packets == 2 && packets.stats().full_events == 1, "packet cap exceeded or lost Full");
    require(packets.pop(output.value, popped, false), "cannot free packet capacity");
    queued(packets, input);
    PacketQueue bytes;
    bytes.reset({0, 16, 0us}, {1,1000});
    require(av_packet_new_side_data(input.value, AV_PKT_DATA_STRINGS_METADATA, 4096), "could not create side data");
    queued(bytes,input); queued(bytes,input);
    const auto byte_full=bytes.push(input.value,Epoch);
    require(byte_full.status==PacketPushStatus::Full && byte_full.limit==PacketQueueLimit::PayloadBytes &&
        bytes.stats().payload_bytes==16 && bytes.stats().peak_payload_bytes==16, "payload bytes counted side data or exceeded cap");
    Packet huge(17);
    const int refs=av_buffer_get_ref_count(huge.value->buf);
    const auto oversized=bytes.push(huge.value,Epoch,PacketPushMode::WaitForSpace);
    require(oversized.status==PacketPushStatus::PacketTooLarge && av_buffer_get_ref_count(huge.value->buf)==refs &&
        bytes.stats().oversized_packets==1 && bytes.stats().full_events==1, "oversized packet waited or retained references");
    PacketQueue time;
    time.reset({0,0,2s},{1,1000});
    input.at(-5000);queued(time,input);input.at(-3000);queued(time,input);
    input.at(-2999);
    const auto span_full=time.push(input.value,Epoch);
    require(span_full.status==PacketPushStatus::Full && span_full.limit==PacketQueueLimit::TimestampSpan &&
        time.stats().timestamp_span_seconds==2.0 && time.stats().withinLimits(), "timestamp boundary exceeded");
    require(time.pop(output.value,popped,false),"cannot free timestamp span");queued(time,input);
    require(time.stats().timestamp_span_seconds==.001,"pop did not reduce span");
    std::cout<<"three_caps=packets,payload,span exact_boundary=1 side_data_excluded=1 oversized_never_waits=1\n";
}

// 缺失/跳变撤销跨度，可信后缀能重新建立跨度；DTS 优先，不把 B 帧 PTS 重排当作跳变。
void testTimestamps() {
    Packet input,output;PlaybackEpoch popped;
    PacketQueue queue;queue.reset({10,80,2s},{1,1000});
    input.at(std::int64_t{1}<<60);queued(queue,input);
    input.value->dts+=100;input.value->pts-=500;queued(queue,input);
    require(queue.stats().timestamp_span_seconds==.1,"large absolute DTS or reordered PTS broke span");
    input.value->pts=input.value->dts=AV_NOPTS_VALUE;queued(queue,input);
    require(!queue.stats().timestamp_span_seconds && queue.stats().payload_bytes==24,"missing timestamp disabled byte accounting");
    input.at(900000);queued(queue,input);
    for(int i=0;i<3;++i)require(queue.pop(output.value,popped,false),"cannot remove untrusted prefix");
    input.at(900100);queued(queue,input);
    require(queue.stats().timestamp_span_seconds==.1,"trusted suffix did not regain span");
    input.at(920100);queued(queue,input);
    require(!queue.stats().timestamp_span_seconds,"large jump was displayed as queue span");
    input.at(920000);queued(queue,input);
    require(!queue.stats().timestamp_span_seconds,"backward timestamp was displayed as queue span");
    queue.reset({2,16,2s},{0,1});queued(queue,input);queued(queue,input);
    require(!queue.stats().timestamp_span_seconds && queue.push(input.value,Epoch).status==PacketPushStatus::Full,
        "invalid time base disabled packet/byte limits");
    queue.reset({3,24,2s},{1,1});
    input.at(std::numeric_limits<std::int64_t>::min()+1);queued(queue,input);
    input.at(std::numeric_limits<std::int64_t>::max());queued(queue,input);
    require(!queue.stats().timestamp_span_seconds,"extreme timestamp delta overflowed");
    queue.reset({4,32,2s},{1,1000});input.at(0);queued(queue,input);
    input.at(1);require(queue.push(input.value,{1,1,2}).status==PacketPushStatus::Queued &&
        !queue.stats().timestamp_span_seconds,"generation changed without resetting span domain");
    std::cout<<"DTS_preferred=1 missing_jump_generation_invalidates_span=1 trusted_suffix_recovers=1 extreme_timestamps_safe=1\n";
}

// 队列只持自己的引用；Full/过大不克隆，abort/reset/析构释放，已经出队的包继续有效。
void testOwnership() {
    Packet input,output;PlaybackEpoch popped;
    const int baseline=av_buffer_get_ref_count(input.value->buf);
    {
        PacketQueue queue;queue.reset({1,8,0us},{1,1000});queued(queue,input);
        require(av_buffer_get_ref_count(input.value->buf)==baseline+1,"queue did not acquire one reference");
        require(queue.push(input.value,Epoch).status==PacketPushStatus::Full &&
            av_buffer_get_ref_count(input.value->buf)==baseline+1,"Full retained a rejected packet");
        require(queue.pop(output.value,popped,false) && popped==Epoch &&
            av_buffer_get_ref_count(input.value->buf)==baseline+1,"pop changed payload reference ownership");
        queued(queue,input);queue.abort();
        require(av_buffer_get_ref_count(input.value->buf)==baseline+1 && queue.stats().payload_bytes==0 &&
            queue.stats().peak_payload_bytes==8,"abort leaked queued references or erased peak history");
        require(output.value->size==8 && !queue.pop(input.value,popped,false),"abort modified in-flight packet");
        queue.reset();require(queue.stats().peak_packets==0 && queue.stats().full_events==0,"reset retained prior counters");
        queued(queue,input);
    }
    require(av_buffer_get_ref_count(input.value->buf)==baseline+1,"destructor retained queued references");
    av_packet_unref(output.value);
    require(av_buffer_get_ref_count(input.value->buf)==baseline,"in-flight packet reference did not release");
    std::cout<<"Full_no_reference=1 abort_reset_destructor_release=1 popped_packet_independent=1\n";
}

// 队满等待不持队列锁，pop 可以继续；取消/结束/刷新都唤醒等待而不必靠超时轮询。
void testBackpressure() {
    for(const int action:{0,1,2,3}) {
        Packet input,output;PlaybackEpoch popped;
        PacketQueue queue;queue.reset({1,8,0us},{1,1000});queued(queue,input);
        auto producer=std::async(std::launch::async,[&]{return queue.push(input.value,Epoch,PacketPushMode::WaitForSpace);});
        if(producer.wait_for(40ms)!=std::future_status::timeout){queue.abort();throw std::runtime_error("backpressure did not wait");}
        const auto start=std::chrono::steady_clock::now();
        if(action==0)require(queue.pop(output.value,popped,false),"consumer could not release space");
        if(action==1)queue.abort();
        if(action==2)queue.finish();
        if(action==3)queue.reset();
        if(producer.wait_for(500ms)!=std::future_status::ready){queue.abort();throw std::runtime_error("producer not woken by lifecycle/pop");}
        const auto result=producer.get();
        require(std::chrono::steady_clock::now()-start<500ms && result.status==(action==0?PacketPushStatus::Queued:
            action==1?PacketPushStatus::Aborted:action==2?PacketPushStatus::Finished:PacketPushStatus::Cancelled),"wrong wake result");
        require(queue.stats().full_events==0,"normal backpressure counted as live Full");
    }
    std::cout<<"backpressure_woken_by=pop,abort,finish,reset polling=0 live_Full_not_incremented=1\n";
}

// 多生产者必须共享原子检查/入队；反复取消后立即 reset 也不能接纳旧生产者。
void testConcurrency() {
    Packet input;PacketQueue queue;queue.reset({13,104,0us},{1,1000});
    std::atomic<bool> go{false};std::atomic<int> queued_count{0},wrong{0};
    std::vector<std::thread> threads;
    for(int i=0;i<8;++i)threads.emplace_back([&]{while(!go.load())std::this_thread::yield();for(int j=0;j<100;++j){
        const auto status=queue.push(input.value,Epoch).status;
        if(status==PacketPushStatus::Queued)++queued_count;else if(status!=PacketPushStatus::Full)++wrong;
    }});
    go=true;for(auto& thread:threads)thread.join();
    require(queued_count==13 && wrong==0 && queue.stats().peak_packets==13 &&
        queue.stats().peak_payload_bytes==104 && queue.stats().full_events==787,"concurrent pushes exceeded a cap");
    queue.abort();
    for(int round=0;round<16;++round){
        queue.reset({1,8,0us},{1,1000});queued(queue,input);
        std::vector<std::future<PacketPushResult>> waiters;
        for(int i=0;i<4;++i)waiters.push_back(std::async(std::launch::async,[&]{return queue.push(input.value,Epoch,PacketPushMode::WaitForSpace);}));
        bool blocked=true;for(auto& waiter:waiters)blocked &= waiter.wait_for(10ms)==std::future_status::timeout;
        queue.abort();queue.reset();
        for(auto& waiter:waiters){
            if(waiter.wait_for(500ms)!=std::future_status::ready){queue.abort();throw std::runtime_error("cancel/reset waiter stuck");}
            const auto status=waiter.get().status;
            require(status==PacketPushStatus::Aborted || status==PacketPushStatus::Cancelled,"old producer entered reset queue");
        }
        require(blocked && queue.stats().packets==0,"cancel/reset test did not block or left stale packet");
    }
    std::cout<<"concurrent_producers=8 exact_cap=13 abort_then_reset_races=64 old_packet_revived=0\n";
}

// finish 排空后结束，空消费者能被入队/取消/刷新唤醒；失败 pop 保留输出参数。
void testLifecycle() {
    for(const int action:{0,1,2,3}) {
        Packet input,output;PacketQueue queue;PlaybackEpoch popped{9,9,9};
        auto consumer=std::async(std::launch::async,[&]{return queue.pop(output.value,popped,true);});
        const bool blocked=consumer.wait_for(25ms)==std::future_status::timeout;
        if(action==0)queued(queue,input);
        if(action==1)queue.abort();
        if(action==2)queue.finish();
        if(action==3)queue.reset();
        if(consumer.wait_for(500ms)!=std::future_status::ready){queue.abort();throw std::runtime_error("consumer not woken");}
        require(blocked && consumer.get()==(action==0) && popped==(action==0?Epoch:PlaybackEpoch{9,9,9}),"consumer wake changed tag or data");
    }
    Packet input,output;PacketQueue queue;PlaybackEpoch popped;queued(queue,input);queue.finish();
    require(queue.push(input.value,Epoch).status==PacketPushStatus::Finished && queue.pop(output.value,popped,true) &&
        !queue.pop(output.value,popped,true),"finish did not drain exactly once");
    queue.reset({}, {1,1000});for(int i=0;i<700;++i)queued(queue,input);
    require(queue.stats().packets==700 && queue.stats().withinLimits(),"local/unbounded queue inherited live cap");
    std::cout<<"consumer_wakes=push,abort,finish,reset finished_queue_drains=1 unbounded_local_policy=1\n";
}
}
int main(int argc,char** argv) {
    try {
        const std::string_view mode=argc==2?argv[1]:"";
        if(mode=="limits")testLimits();else if(mode=="timestamps")testTimestamps();else if(mode=="ownership")testOwnership();
        else if(mode=="backpressure")testBackpressure();else if(mode=="concurrency")testConcurrency();else if(mode=="lifecycle")testLifecycle();
        else throw std::runtime_error("expected limits, timestamps, ownership, backpressure, concurrency or lifecycle");
        return 0;
    }catch(const std::exception& error){std::cerr<<"Queue test failed: "<<error.what()<<'\n';return 1;}
}
