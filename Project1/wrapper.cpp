#include "wrapper.h"
#include "iostream"
#include "fstream"
#include "cJSON-1.7.14/cJSON.h"
#include "utils.h"
#include "project.h"
#include "debug.h"
#include "vector"
#include "dir_utils.h"
#include "direct.h" // mkdir
#include "archive.h"
#include "magic.h"
#include "md5/md5.h"
#include "single_ins.h"

#include "wrapper.func.cpp"

using namespace std;

// 从metadata里获取optiondata
optiondata get_optiondata(cJSON* metadata)
{
    bool check_hash = cJSON_IsTrue(cJSON_GetObjectItem(metadata, "check_hash"));
    string exec = cJSON_GetObjectItem(metadata, "exec")->valuestring;
    bool show_console = cJSON_IsTrue(cJSON_GetObjectItem(metadata, "show_console"));

    optiondata od;
    od.exec = exec;
    od.check_hash = check_hash;
    od.show_console = show_console;

    return od;
}

// 读取metadata
// fileIn：从那个文件读取
// out_addr：metadata在那个位置
// out_len：metadata的长度
// out_metadata：metadata输出
void lw_read_metadata(string fileIn, cJSON** out_metadata, size_t* out_addr, size_t* out_len)
{
    fstream fin(fileIn, fstream::in | fstream::binary);
    error_check(!fin.fail(), "could not open the file to extract: " + fileIn);

    // 读取jumpdata
    size_t jumpdata_offset = get_magic_offset(fin, (uint8_t*)MAGIC_HEADER, MAGIC_LEN) + MAGIC_LEN;

    if (jumpdata_offset == 0)
        throw jumpdata_not_found_exception();

    fin.seekg(jumpdata_offset);
    char* jumpdata = new char[PRESERVE_LEN - MAGIC_LEN];
    fin.read((char*)jumpdata, PRESERVE_LEN - MAGIC_LEN);
    error_check(!fin.bad(), "could not read the jumpdata: " + fileIn);

    // 解析jumpdata
    cJSON* json = cJSON_Parse(jumpdata);
    if (json == nullptr)
    {
        delete[] jumpdata;
        throw jumpdata_invaild_exception();
    }
    size_t metadata_addr = cJSON_GetObjectItem(json, "offset")->valueint;
    size_t metadata_len = cJSON_GetObjectItem(json, "len")->valueint;
    cJSON_Delete(json);
    delete[] jumpdata;

    if (metadata_addr == 0 || metadata_len == 0)
        throw metadata_not_found_exception();

    // 读取metadata
    fin.clear();
    fin.seekg(metadata_addr);

    char* meta_buf = new char[metadata_len + 1];
    //memset(meta_buf, 0, metadata_len2);
    fin.read(meta_buf, metadata_len + 1);
    error_check(!fin.bad(), "could not read the metadata: " + fileIn);

    printf("metadata offset: 0x%llx, len: %lld\n", metadata_addr, metadata_len);

    // 解析metadata
    cJSON* meta_json = cJSON_Parse(meta_buf);
    delete[] meta_buf;
    if (meta_json == nullptr)
        throw metadata_invaild_exception();;

    // 传出数据
    if (out_metadata != nullptr)
        *out_metadata = meta_json;
    else
        cJSON_Delete(meta_json);

    if (out_addr)
        *out_addr = metadata_addr;

    if (out_len)
        *out_len = metadata_len;

    // 释放资源
    fin.close();
}

// 打包文件夹
void lw_pack(string fileIn, string fileOut, string source_dir, string temp_compressed_dir, optiondata& optdata)
{
    // 创建文件夹
    string parent_dir = get_dir_name(fileOut);
    if (!file_exists(parent_dir))
        error_check(!_mkdir(parent_dir.c_str()), "pack_binaries: could not create the parent dir of the output-file: " + parent_dir);

    std::fstream fin(fileIn, std::fstream::in | std::fstream::binary);
    std::fstream fout(fileOut, std::fstream::out | std::fstream::binary | std::fstream::trunc);
    error_check(!fin.fail(), "pack_binaries: could not open the in-file: " + fileIn);
    error_check(!fout.fail(), "pack_binaries: could not open the out-file: " + fileOut);

    // 获取magic位置
    auto magic_offset = get_magic_offset(fin, (uint8_t*)MAGIC_HEADER, MAGIC_LEN);
    if (magic_offset == 0)
    {
        printf("Can not locate the magic-header in the executable file.\n");
        return;
    }
    printf("magic at: %llx\n", magic_offset);

    // 复制源文件
    int buf_size = 4 * 1024;
    uint8_t* buf = new uint8_t[buf_size];

    streamsize readBytes = 0;
    do {
        fin.read((char*)buf, buf_size);
        error_check(!fin.bad(), "pack_binaries: could not copy the binary: read");
        readBytes = fin.gcount();
        fout.write((char*)buf, readBytes);
        error_check(!fout.bad(), "pack_binaries: could not copy the binary: write");
    } while (readBytes > 0);

    // 准备临时目录用来存放压缩后的数据
    if (!file_exists(temp_compressed_dir))
        error_check(!_mkdir(temp_compressed_dir.c_str()), "pack_binaries: could not create the temp dir: " + temp_compressed_dir);
    printf("tempdir: %s\n", temp_compressed_dir.c_str());

    // 写metadata
    printf("generate directory strcuture for %s\n", source_dir.c_str());
    auto metadata_pair = pack_binaries(fout, source_dir, temp_compressed_dir, optdata);
    size_t metadata_addr = metadata_pair.first;
    size_t metatada_len = metadata_pair.second;

    // 写jumpdata
    uint64_t jumpdata_addr = magic_offset + MAGIC_LEN;
    string jumpdata = generate_jumpdata(metadata_addr, metatada_len);
    fout.seekp(jumpdata_addr);
    fout.write(jumpdata.c_str(), jumpdata.length());

    printf("wrote metadate at: %llx, magic at: %llx\n", metadata_addr, magic_offset);
    printf("wrote jumpdata data: 0x%llx, or %lld\n", jumpdata_addr, jumpdata_addr);

    fin.close();
    fout.close();
    delete[] buf;
}

// 解压数据
void lw_extract(string fileIn, string extract_dir, bool single_ins_protection)
{
    // 读取metadata
    size_t metadata_addr;
    size_t metadata_len;
    cJSON* meta;

    lw_read_metadata(fileIn, &meta, &metadata_addr, &metadata_len);

    optiondata opt = get_optiondata(meta);
    cJSON* directories = cJSON_GetObjectItem(meta, "directories");
    cJSON* addr_table = cJSON_GetObjectItem(meta, "address_table");

    // 单实例保护，当有多个实例存在时，后创建的实例不解压数据，直接运行就好，防止对先创建的运行中的实例造成文件破坏
    string write_protect_key = string("lw-sil-") + get_string_md5(extract_dir);
    printf("wtkey: %s\n", write_protect_key.c_str());
    bool write_protect = !request_single_instance_lock(write_protect_key);

    if (!write_protect)
    {
        // 准备解压数据
        fstream fin(fileIn, fstream::in | fstream::binary);
        error_check(!fin.fail(), "extract_binaries: could not open the file to extract: " + fileIn);

        // 建立解压输出目录
        string decompressed = extract_dir;
        if (!file_exists(decompressed))
            error_check(!_mkdir(decompressed.c_str()), "extract_binaries: could not create the extract-dir: " + decompressed);

        // 建立所有的文件夹(directories字段)
        for (int i = 0; i < cJSON_GetArraySize(directories); i++)
        {
            string dir = cJSON_GetArrayItem(directories, i)->valuestring;
            printf("mkdir: %s\n", dir.c_str());

            string cdir = decompressed + "\\" + string_replace(dir, "/", "\\");
            if (!file_exists(cdir))
                error_check(!_mkdir(cdir.c_str()), "extract_binaries: could not create the dir by the bounds: " + decompressed);
        }

        // 解压所有打包好的文件
        fin.clear();
        fin.seekg(metadata_addr);
        size_t base_addr = metadata_addr + metadata_len + split_block_len; // 末尾有8个是分隔符（都是0）

        printf("\nBaseOffset: 0x%llx\n", base_addr);
        printf("CheckHash: %s\n", opt.check_hash?"check":"no_check");

        for (int i = 0; i < cJSON_GetArraySize(addr_table); i++)
        {
            cJSON* item = cJSON_GetArrayItem(addr_table, i);
            string key = item->string;
            string raw_path = cJSON_GetObjectItem(item, "raw_path")->valuestring;
            size_t raw_size = cJSON_GetObjectItem(item, "raw_size")->valueint;
            string raw_hash = cJSON_GetObjectItem(item, "raw_hash")->valuestring;
            size_t offset = cJSON_GetObjectItem(item, "offset")->valueint;
            size_t length = cJSON_GetObjectItem(item, "len")->valueint;
            string hash = cJSON_GetObjectItem(item, "hash")->valuestring;

            string target_file = decompressed + "\\" + string_replace(raw_path, "/", "\\");
            size_t addr = base_addr + offset;

            // 校验
            if (opt.check_hash)
            {
                fin.clear();
                fin.seekg(addr);
                string md5 = get_stream_md5(fin, length);
                if (md5 != hash)
                {
                    printf("\nhash-check not passed, the file might be damaged!\nfile: %s\n", raw_path.c_str());
                    printf("hash-inside: %s\nhash-calculated: %s\n", hash.c_str(), md5.c_str());
                    printf("address: 0x%llx\nlength: %lld\n", addr, length);
                    throw binaries_damaged_exception();
                }
            }

            // 如果文件大小和校验一样，则跳过解压，重复使用
            if (file_exists(target_file) && get_file_length(target_file) == raw_size && (!opt.check_hash || get_file_md5(target_file) == raw_hash))
            {
                printf("reuse: %s\n", raw_path.c_str());
                continue;
            }

            printf("decompress: %s, offset: 0x%llx, len: %lld\n", raw_path.c_str(), addr, length);
            // 解压
            inflate_to_file(fin, addr, length, target_file);
        }

        fin.close();
    } else {
        printf("muilt instance is detected.\n");
    }

    // 释放资源
    cJSON_Delete(meta);
}

// 详细信息
void lw_detail(string fileIn, string export_file)
{
    // 读取metadata
    size_t metadata_addr;
    size_t metadata_len;
    cJSON* meta;
    lw_read_metadata(fileIn, &meta, &metadata_addr, &metadata_len);

    // 打印metadata
    char* pretty = cJSON_Print(meta);

    if (export_file == "")
    {
        // 输出基本信息
        optiondata od = get_optiondata(meta);
        printf("detail:\n  check_hash: %d\n  exec: %s\n  show_console: %d\n----------\n%s\n----------\n", od.check_hash, od.exec.c_str(), od.show_console, pretty);

        // 计算基本偏移地址
        size_t base_addr = metadata_addr + metadata_len + split_block_len; // 末尾有8个是分隔符（都是0）
        printf("BaseAddr: 0x%llx\n", base_addr);

        // 输出所有目录
        cJSON* directories = cJSON_GetObjectItem(meta, "directories");
        for (int i = 0; i < cJSON_GetArraySize(directories); i++)
        {
            string path = cJSON_GetArrayItem(directories, i)->valuestring;
            printf("Directory: %s\n", path.c_str());
        }

        // 输出所有文件
        cJSON* addr_table = cJSON_GetObjectItem(meta, "address_table");
        for (int i = 0; i < cJSON_GetArraySize(addr_table); i++)
        {
            cJSON* item = cJSON_GetArrayItem(addr_table, i);
            string key = item->string;
            string path = cJSON_GetObjectItem(item, "raw_path")->valuestring;
            size_t offset = cJSON_GetObjectItem(item, "offset")->valueint;
            size_t length = cJSON_GetObjectItem(item, "len")->valueint;
            string hash = cJSON_GetObjectItem(item, "hash")->valuestring;
            size_t addr = base_addr + offset;

            printf("File: %s, offset: 0x%llx, len: %lld, %s\n", path.c_str(), addr, length, hash.c_str());
        }

        printf("----------\n");
    } else {
        std::fstream fout(export_file, std::fstream::out | std::fstream::trunc);
        error_check(!fout.fail(), "detail_binaries: could not open the export-file: " + export_file);
        fout.write(pretty, strlen(pretty));
        error_check(!fout.fail(), "detail_binaries: could not write to the export-file: " + export_file);
        fout.close();
        printf("detail wrote: %s\n", export_file.c_str());
    }

    cJSON_Delete(meta);
    delete[] pretty;
}