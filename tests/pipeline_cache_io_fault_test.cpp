// Authored native syscall refusals around the actual production helper.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <cstdint>
#include <cerrno>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif
enum Fault { NONE, CREATE, RESIZE, ALLOCATE, LOCK, MAP, VIEW, FLUSH, UNMAP, MODE, TOUCH, SYNC, CLOSE, REPLACE, TRUNCATE, CLEANUP };
static Fault fault=NONE;
static bool injected=false;
static bool refuse(Fault kind){if(fault==kind&&!injected){injected=true;return true;}return false;}
#ifdef _WIN32
static HANDLE io_create(LPCWSTR p,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE t){
    if(refuse(CREATE))return INVALID_HANDLE_VALUE;return CreateFileW(p,access,share,sa,disposition,flags,t);
}
static BOOL io_resize(HANDLE f){if(refuse(RESIZE)||refuse(TRUNCATE))return FALSE;return SetEndOfFile(f);}
static HANDLE io_mapping(HANDLE f,LPSECURITY_ATTRIBUTES sa,DWORD protection,DWORD high,DWORD low,LPCWSTR name){
    if(refuse(MAP))return nullptr;return CreateFileMappingW(f,sa,protection,high,low,name);
}
static LPVOID io_view(HANDLE m,DWORD access,DWORD high,DWORD low,SIZE_T n){if(refuse(VIEW))return nullptr;return MapViewOfFile(m,access,high,low,n);}
static BOOL io_flush(LPCVOID p,SIZE_T n){if(refuse(FLUSH))return FALSE;return FlushViewOfFile(p,n);}
static BOOL io_unmap(LPCVOID p){if(refuse(UNMAP))return FALSE;return UnmapViewOfFile(p);}
static BOOL io_sync(HANDLE f){if(refuse(SYNC))return FALSE;return FlushFileBuffers(f);}
static BOOL io_time(HANDLE f,const FILETIME* a,const FILETIME* b,const FILETIME* c){if(refuse(TOUCH))return FALSE;return SetFileTime(f,a,b,c);}
static BOOL io_close_file(HANDLE f){if(GetFileType(f)==FILE_TYPE_DISK&&refuse(CLOSE))return FALSE;return CloseHandle(f);}
static BOOL io_replace(LPCWSTR a,LPCWSTR b,DWORD flags){if(refuse(REPLACE))return FALSE;return MoveFileExW(a,b,flags);}
static BOOL io_remove(LPCWSTR p){if(refuse(CLEANUP)){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return DeleteFileW(p);}
#define CreateFileW io_create
#define SetEndOfFile io_resize
#define CreateFileMappingW io_mapping
#define MapViewOfFile io_view
#define FlushViewOfFile io_flush
#define UnmapViewOfFile io_unmap
#define FlushFileBuffers io_sync
#define SetFileTime io_time
#define CloseHandle io_close_file
#define MoveFileExW io_replace
#define DeleteFileW io_remove
#else
static int io_create(char* path){if(refuse(CREATE))return -1;return mkstemp(path);}
static int io_resize(int fd,off_t n){if(refuse(RESIZE)||refuse(TRUNCATE))return -1;return ftruncate(fd,n);}
[[maybe_unused]] static int io_allocate(int fd,off_t start,off_t n){if(refuse(ALLOCATE))return ENOSPC;return posix_fallocate(fd,start,n);}
[[maybe_unused]] static int io_lock(int fd,int op){if(refuse(LOCK)){errno=EWOULDBLOCK;return -1;}return flock(fd,op);}
[[maybe_unused]] static int io_time(int fd,const timespec times[2]){if(refuse(TOUCH))return -1;return futimens(fd,times);}
[[maybe_unused]] static int io_mode(int fd,mode_t mode){if(refuse(MODE))return -1;return fchmod(fd,mode);}
static int map_calls=0;
static void* io_view(void* a,size_t n,int prot,int flags,int fd,off_t at){++map_calls;if(refuse(VIEW))return MAP_FAILED;return mmap(a,n,prot,flags,fd,at);}
static int io_flush(void* p,size_t n,int flags){if(refuse(FLUSH))return -1;return msync(p,n,flags);}
static int io_unmap(void* p,size_t n){if(refuse(UNMAP))return -1;return munmap(p,n);}
static int io_sync(int fd){if(refuse(SYNC))return -1;return fsync(fd);}
static int io_close_file(int fd){if(refuse(CLOSE)){::close(fd);return -1;}return ::close(fd);}
static int io_replace(const char* a,const char* b){if(refuse(REPLACE))return -1;return rename(a,b);}
static int io_remove(const char* p){if(refuse(CLEANUP)){errno=EACCES;return -1;}return unlink(p);}
#define mkstemp io_create
#define ftruncate io_resize
#define posix_fallocate io_allocate
#define flock io_lock
#define futimens io_time
#define fchmod io_mode
#define mmap io_view
#define msync io_flush
#define munmap io_unmap
#define fsync io_sync
#define close io_close_file
#define rename io_replace
#define unlink io_remove
#endif
#include "pipeline_cache_file.h"
#ifdef _WIN32
#undef CreateFileW
#undef SetEndOfFile
#undef CreateFileMappingW
#undef MapViewOfFile
#undef FlushViewOfFile
#undef UnmapViewOfFile
#undef FlushFileBuffers
#undef SetFileTime
#undef CloseHandle
#undef MoveFileExW
#undef DeleteFileW
#else
#undef mkstemp
#undef ftruncate
#undef posix_fallocate
#undef flock
#undef futimens
#undef fchmod
#undef mmap
#undef msync
#undef munmap
#undef fsync
#undef close
#undef rename
#undef unlink
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char** argv){
    CHECK(argc==2);std::string path=std::string(argv[1])+"/fault-cache.bin";
    auto valid=[](const uint8_t* p,uint64_t n){return n==64&&p[0]==42;};
    auto seed=[&](){auto f=mkwvk::PipelineCacheFile::writable(path,64);if(f.empty())return false;memset(f.data(),42,64);return f.publish(64);};
    CHECK(seed());int cases=0;
    for(Fault f:{CREATE,RESIZE,ALLOCATE,LOCK,MAP,VIEW,FLUSH,UNMAP,MODE,TOUCH,SYNC,CLOSE,REPLACE,TRUNCATE}){
#ifdef _WIN32
        if(f==ALLOCATE||f==LOCK||f==MODE)continue; // POSIX reservation/lock/mode have their own failure contract.
#else
        int maps_before=map_calls;
#endif
#ifndef _WIN32
        if(f==MAP)continue; // POSIX has one mmap operation, no separate io_mapping handle.
#endif
        bool acquisition=f==CREATE||f==RESIZE||f==ALLOCATE||f==LOCK||f==MAP||f==VIEW;
        fault=acquisition?f:NONE;injected=false;
        {
            auto output=mkwvk::PipelineCacheFile::writable(path,128);
            if(acquisition){CHECK(output.empty());}
            else {CHECK(!output.empty());memset(output.data(),99,128);fault=f;CHECK(!output.publish(96));}
#ifndef _WIN32
            if(f==ALLOCATE)CHECK(map_calls==maps_before);
#endif
        }
        CHECK(injected);fault=NONE;
        auto old=mkwvk::PipelineCacheFile::read(path,valid);CHECK(!old.empty());CHECK(old.data()[63]==42);old.reset();
        CHECK(seed()); // Same output can be saved again after cleanup.
        for(const auto& e:std::filesystem::directory_iterator(argv[1]))CHECK(e.path().filename().string().find(".tmp-")==std::string::npos);
        ++cases;
    }
#ifdef _WIN32
    fault=CREATE;
#else
    fault=VIEW;
#endif
    injected=false;
    auto unavailable=mkwvk::PipelineCacheFile::read(path,valid);
    CHECK(injected&&unavailable.empty()&&!unavailable.rejected());
    fault=NONE;CHECK(!mkwvk::PipelineCacheFile::read(path,valid).empty());
#ifndef _WIN32
    fault=CREATE;injected=false;
    {auto direct=mkwvk::PipelineCacheFile::read(path,valid);CHECK(!direct.empty()&&!injected);}
    fault=NONE; // Reads do not create or write a sibling snapshot.
#endif
    auto abandoned=mkwvk::PipelineCacheFile::writable(path,128);CHECK(!abandoned.empty());
    fault=CLEANUP;injected=false;CHECK(!abandoned.reset()&&injected);
    fault=NONE;CHECK(abandoned.reset());
    for(const auto& e:std::filesystem::directory_iterator(argv[1]))CHECK(e.path().filename().string().find(".tmp-")==std::string::npos);
    printf("{\"nativeFailureStages\":%d,\"oldOutputPreserved\":true,\"retryPassed\":true,\"ownedTemporariesCleaned\":true,\"readIoFailureNotIncompatible\":true}\n",cases);
    return 0;
}
