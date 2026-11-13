// Authored tests of the production storage helper; no Vulkan/game data.
#include "pipeline_cache_file.h"
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <set>
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void cleanup_cases(const std::filesystem::path& parent, int& checks) {
    namespace fs=std::filesystem;
    const auto root=parent/fs::path(u8"cleanup-\xC3\xA9");CHECK(fs::create_directory(root));
    const auto encoded=root.u8string();const std::string folder(reinterpret_cast<const char*>(encoded.data()),encoded.size());
    const auto old=fs::file_time_type::clock::now()-std::chrono::hours(48);
    auto seed=[&](const char* name,bool aged){const auto p=root/name;std::ofstream out(p,std::ios::binary);out<<"keep";out.close();CHECK(out.good());if(aged)fs::last_write_time(p,old);return p;};
    const auto abandoned=seed("mkw_vk_pipelines.bin.tmp-aB12cD",true);
    const auto play=seed("mkw_vk_pipelines_play_7.bin.tmp-1234-99",true);
    const auto young=seed("mkw_vk_pipelines.bin.tmp-12-1",false);
    const auto other=seed("other.bin.tmp-abcdef",true);
    const auto malformed=seed("mkw_vk_pipelines_play_x.bin.tmp-abcdef",true);
    const auto suffix=seed("mkw_vk_pipelines.bin.tmp-not-temporary",true);
    const auto published=seed("mkw_vk_pipelines.bin",true);
    const auto directory=root/"mkw_vk_pipelines.bin.tmp-44-3";CHECK(fs::create_directory(directory));fs::last_write_time(directory,old);
    const auto target=seed("target",true), hard=root/"mkw_vk_pipelines.bin.tmp-a1B2c3";
    fs::create_hard_link(target,hard);
#ifndef _WIN32
    const auto link=root/"mkw_vk_pipelines.bin.tmp-a2B3c4";CHECK(symlink(target.c_str(),link.c_str())==0);
#endif
    CHECK(mkwvk::PipelineCacheFile::cleanup_temporaries(folder)==2);
    CHECK(!fs::exists(abandoned)&&!fs::exists(play));
    for(const auto& p:{young,other,malformed,suffix,published,directory,target,hard})CHECK(fs::exists(p));
#ifndef _WIN32
    CHECK(fs::is_symlink(link)&&fs::exists(target));
#endif
    std::set<fs::path> before;for(const auto& entry:fs::directory_iterator(root))before.insert(entry.path());
    const auto bytes=published.u8string();const std::string destination(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    auto writer=mkwvk::PipelineCacheFile::writable(destination,128);CHECK(!writer.empty());
    fs::path live;for(const auto& entry:fs::directory_iterator(root))if(!before.count(entry.path()))live=entry.path();
    CHECK(!live.empty());fs::last_write_time(live,old);
    CHECK(mkwvk::PipelineCacheFile::cleanup_temporaries(folder)==0&&fs::exists(live));
    memset(writer.data(),42,128);CHECK(writer.publish(64));
    CHECK(!fs::exists(live)&&fs::file_size(published)==64);
    CHECK(fs::last_write_time(published)>old+std::chrono::hours(24));
    CHECK(mkwvk::PipelineCacheFile::cleanup_temporaries(folder)==0);
}
int main(int argc,char** argv){
    int checks=0;CHECK(argc==2);
    std::string path=std::string(argv[1])+"/owned-\xC3\xA9-cache.bin";
    auto valid=[](const uint8_t* h,uint64_t n){return n==128 && h[0]==42;};
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).empty());
    CHECK(!mkwvk::PipelineCacheFile::read(path,valid).rejected());
    {
        auto out=mkwvk::PipelineCacheFile::writable(path,256);CHECK(!out.empty());
        memset(out.data(),42,256);CHECK(!out.publish(257));CHECK(out.publish(128));
    }
    {
        auto in=mkwvk::PipelineCacheFile::read(path,valid);CHECK(in.size()==128);
        for(size_t i=0;i<in.size();++i)CHECK(in.data()[i]==42);
        // A discarded producer leaves the old destination unchanged.
        {auto abandoned=mkwvk::PipelineCacheFile::writable(path,512);CHECK(!abandoned.empty());memset(abandoned.data(),99,512);}
        auto moved=std::move(in);CHECK(in.empty());CHECK(moved.size()==128);
        auto replaced=mkwvk::PipelineCacheFile::writable(path,256);CHECK(!replaced.empty());memset(replaced.data(),43,256);
#if defined(_WIN32)
        CHECK(!replaced.publish(128)); // Windows refuses replacing an active mapped destination.
#else
        CHECK(replaced.publish(128));
#endif
        CHECK(moved.data()[0]==42); // Existing view retains old bytes.
    }
#if defined(_WIN32)
    CHECK(!mkwvk::PipelineCacheFile::read(path,valid).empty());
    {auto retry=mkwvk::PipelineCacheFile::writable(path,128);CHECK(!retry.empty());memset(retry.data(),43,128);CHECK(retry.publish(128));}
#endif
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).empty());
    CHECK(mkwvk::PipelineCacheFile::read(path,valid).rejected());
    auto current=[](const uint8_t* h,uint64_t n){return n==128&&h[0]==43;};
    CHECK(!mkwvk::PipelineCacheFile::read(path,current).empty());
    CHECK(mkwvk::PipelineCacheFile::writable(path,0).empty());
    CHECK(mkwvk::PipelineCacheFile::read(std::string(argv[1]),current).empty());
#if !defined(_WIN32)
    {
        auto mapped=mkwvk::PipelineCacheFile::read(path,current);CHECK(!mapped.empty());
        CHECK(unlink(path.c_str())==0);CHECK(mapped.data()[127]==43);
    }
    CHECK(mkwvk::PipelineCacheFile::read(path,current).empty());
    {auto restored=mkwvk::PipelineCacheFile::writable(path,128);CHECK(!restored.empty());memset(restored.data(),43,128);CHECK(restored.publish(128));}
    std::string link=path+".link";CHECK(symlink(path.c_str(),link.c_str())==0);
    {auto linked=mkwvk::PipelineCacheFile::read(link,current);CHECK(linked.size()==128&&linked.data()[127]==43);}
    CHECK(unlink(link.c_str())==0);
    std::string pipe=path+".pipe";CHECK(mkfifo(pipe.c_str(),0600)==0);
    CHECK(mkwvk::PipelineCacheFile::read(pipe,current).empty());CHECK(unlink(pipe.c_str())==0);
    CHECK(chmod(path.c_str(),0640)==0);
    {auto saved=mkwvk::PipelineCacheFile::writable(path,128);CHECK(!saved.empty());memset(saved.data(),43,128);CHECK(saved.publish(128));}
    struct stat mode{};CHECK(stat(path.c_str(),&mode)==0&&(mode.st_mode&0777)==0640);
    const mode_t previous=umask(0077);
    const std::string fresh=path+".private";
    {auto saved=mkwvk::PipelineCacheFile::writable(fresh,128);CHECK(!saved.empty());memset(saved.data(),43,128);CHECK(saved.publish(128));}
    umask(previous);CHECK(stat(fresh.c_str(),&mode)==0&&(mode.st_mode&0777)==0600);
#endif
    size_t leaked=0;
    for(const auto& entry:std::filesystem::directory_iterator(argv[1]))if(entry.path().filename().string().find(".tmp-")!=std::string::npos)++leaked;
    CHECK(leaked==0);
    const std::string argument(argv[1]);const std::u8string parent(argument.begin(),argument.end());
    cleanup_cases(std::filesystem::path(parent),checks);
    printf("{\"assertions\":%d,\"productionHelper\":true,\"passed\":true}\n",checks);
    return 0;
}
