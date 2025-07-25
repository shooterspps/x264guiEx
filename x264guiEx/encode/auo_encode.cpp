﻿// -----------------------------------------------------------------------------------------
// x264guiEx/x265guiEx/svtAV1guiEx/ffmpegOut/QSVEnc/NVEnc/VCEEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2010-2022 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cmath>
#include <float.h>
#include <stdio.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <memory>
#include <functional>

#include "auo.h"
#include "auo_version.h"
#include "auo_util.h"
#include "auo_conf.h"
#include "auo_settings.h"
#include "auo_system.h"
#include "auo_pipe.h"
#include "auo_mes.h"

#include "auo_frm.h"
#include "auo_video.h"
#include "auo_encode.h"
#include "auo_error.h"
#include "auo_audio.h"
#include "auo_faw2aac.h"
#include "cpu_info.h"
#include "exe_version.h"

using unique_handle = std::unique_ptr<std::remove_pointer<HANDLE>::type, std::function<void(HANDLE)>>;

static void create_aviutl_opened_file_list(PRM_ENC *pe);
static bool check_file_is_aviutl_opened_file(const char *filepath, const PRM_ENC *pe);

static void avoid_exsisting_tmp_file(char *buf, size_t size) {
    if (!PathFileExists(buf)) {
        return;
    }
    char tmp[MAX_PATH_LEN];
    for (int i = 0; i < 1000000; i++) {
        char new_ext[32];
        sprintf_s(new_ext, ".%d%s", i, PathFindExtension(buf));
        strcpy_s(tmp, buf);
        change_ext(tmp, size, new_ext);
        if (!PathFileExists(tmp)) {
            strcpy_s(buf, size, tmp);
            return;
        }
    }
}

#if ENCODER_X264 || ENCODER_X265 || ENCODER_SVTAV1
#pragma warning (push)
#pragma warning (disable: 4244)
#pragma warning (disable: 4996)
static inline std::string tolowercase(const std::string& str) {
    std::string str_copy = str;
    std::transform(str_copy.cbegin(), str_copy.cend(), str_copy.begin(), tolower);
    return str_copy;
}
#pragma warning (pop)
#endif

static std::vector<std::filesystem::path> find_exe_files(const char *target_dir) {
    std::vector<std::filesystem::path> ret;
    try {
        for (const std::filesystem::directory_entry& x : std::filesystem::recursive_directory_iterator(target_dir)) {
            if (x.path().extension() == ".exe") {
                ret.push_back(x.path());
            }
        }
    } catch (...) {}
    return ret;
}

static std::vector<std::filesystem::path> find_exe_files(const char *target_dir, const char *target_dir2) {
    auto list1 = find_exe_files(target_dir);
    auto list2 = find_exe_files(target_dir2);
    list1.insert(list1.end(), list2.begin(), list2.end());
    return list1;
}

static std::vector<std::filesystem::path> find_target_exe_files(const char *target_name, const std::vector<std::filesystem::path>& exe_files) {
    std::vector<std::filesystem::path> ret;
    const auto targetNameLower = tolowercase(std::filesystem::path(target_name).stem().string());
    for (const auto& path : exe_files) {
        if (tolowercase(path.stem().string()).substr(0, targetNameLower.length()) == targetNameLower) {
            ret.push_back(path);
        }
    }
    return ret;
}

static bool ends_with(const std::string& s, const std::string& check) {
    if (s.size() < check.size()) return false;
    return std::equal(std::rbegin(check), std::rend(check), std::rbegin(s));
}

static std::vector<std::filesystem::path> select_exe_file(const std::vector<std::filesystem::path>& pathList) {
    if (pathList.size() <= 1) {
        return pathList;
    }
    std::vector<std::filesystem::path> exe32bit;
    std::vector<std::filesystem::path> exe64bit;
    std::vector<std::filesystem::path> exeUnknown;
    for (const auto& path : pathList) {
        if (ends_with(tolowercase(path.filename().string()), "_x64.exe")) {
            exe64bit.push_back(path);
            continue;
        } else if (ends_with(tolowercase(path.filename().string()), "_x86.exe")) {
            exe32bit.push_back(path);
            continue;
        }
        bool checked = false;
        std::filesystem::path p = path;
        for (int i = 0; p.string().length() > 0 && i < 10000; i++) {
            auto parent = p.parent_path();
            if (parent == p) {
                break;
            }
            if (p.filename().string() == "x64") {
                exe64bit.push_back(path);
                checked = true;
                break;
            } else if (p.filename().string() == "x86") {
                exe32bit.push_back(path);
                checked = true;
                break;
            }
        }
        if (!checked) {
            if (ends_with(tolowercase(path.filename().string()), "64.exe")) {
                exe64bit.push_back(path);
            } else {
                exeUnknown.push_back(path);
            }
        }
    }
    if (is_64bit_os()) {
        return (exe64bit.size() > 0) ? exe64bit : exeUnknown;
    } else {
        return (exe32bit.size() > 0) ? exe32bit : exeUnknown;
    }
}

std::filesystem::path find_latest_videnc(const std::vector<std::filesystem::path>& pathList) {
    if (pathList.size() == 0) {
        return std::filesystem::path();
    }
    auto selectedPathList = select_exe_file(pathList);
    if (selectedPathList.size() == 1) {
        return selectedPathList.front();
    }
    int version[4] = { 0 };
    std::filesystem::path ret;
    for (auto& path : selectedPathList) {
        int value[4] = { 0 };
#if ENCODER_X264
        value[0] = get_x264_rev(path.string().c_str());
        if (value[0] >= version[0]) {
            version[0] = value[0];
            ret = path;
    	}
#elif ENCODER_X265
        if (get_x265_rev(path.string().c_str(), value) == 0) {
            if (version_a_larger_than_b(value, version) > 0) {
                memcpy(version, value, sizeof(version));
                ret = path;
            }
        }
#elif ENCODER_SVTAV1
        if (get_svtav1_rev(path.string().c_str(), value) == 0) {
            if (version_a_larger_than_b(value, version) > 0) {
                memcpy(version, value, sizeof(version));
                ret = path;
            }
        }
#elif ENCODER_QSV || ENCODER_NVENC || ENCODER_VCEENC
        if (get_exe_version_info(path.string().c_str(), value) == 0) {
            if (version_a_larger_than_b(value, version) > 0) {
                memcpy(version, value, sizeof(version));
                ret = path;
            }
        }
#else
		static_assert(false);
#endif
    }
    return ret;
}

static BOOL check_muxer_exist(MUXER_SETTINGS *muxer_stg, const char *aviutl_dir, const BOOL get_relative_path, const std::vector<std::filesystem::path>& exe_files) {
    if (PathFileExists(muxer_stg->fullpath)) {
        info_use_exe_found(muxer_stg->dispname, muxer_stg->fullpath);
        return TRUE;
    }
    const auto targetExes = select_exe_file(find_target_exe_files(muxer_stg->filename, exe_files));
    if (targetExes.size() > 0) {
        if (get_relative_path) {
            GetRelativePathTo(muxer_stg->fullpath, _countof(muxer_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
        } else {
            strcpy_s(muxer_stg->fullpath, targetExes.front().string().c_str());
        }
    }
    if (PathFileExists(muxer_stg->fullpath)) {
        info_use_exe_found(muxer_stg->dispname, muxer_stg->fullpath);
        return TRUE;
    }
    error_no_exe_file(muxer_stg->dispname, muxer_stg->fullpath);
    return FALSE;
}

static BOOL check_if_exe_is_mp4box(const char *exe_path, const char *version_arg) {
    BOOL ret = FALSE;
    char exe_message[8192] = { 0 };
    if (   PathFileExists(exe_path)
        && RP_SUCCESS == get_exe_message(exe_path, version_arg, exe_message, _countof(exe_message), AUO_PIPE_MUXED)
        && (stristr(exe_message, "mp4box") || stristr(exe_message, "GPAC"))) {
        ret = TRUE;
    }
    return ret;
}

static BOOL check_if_exe_is_lsmash(const char *exe_path, const char *version_arg) {
    BOOL ret = FALSE;
    char exe_message[8192] = { 0 };
    if (   PathFileExists(exe_path)
        && RP_SUCCESS == get_exe_message(exe_path, version_arg, exe_message, _countof(exe_message), AUO_PIPE_MUXED)
        && stristr(exe_message, "L-SMASH")) {
        ret = TRUE;
    }
    return ret;
}

static BOOL check_muxer_matched_with_ini(const MUXER_SETTINGS *mux_stg) {
    BOOL ret = TRUE;
    //不確定な場合は"0", mp4boxなら"-1", L-SMASHなら"1"
    if (ENCODER_X264 || ENCODER_X265) {
        bool mp4box_ini = stristr(mux_stg[MUXER_MP4].filename, "mp4box") != nullptr;
        if (mp4box_ini) {
            error_mp4box_ini();
            ret = FALSE;
        }
    }
    return ret;
}

bool is_afsvfr(const CONF_GUIEX *conf) {
    return conf->vid.afs != 0;
}

static BOOL check_amp(CONF_GUIEX *conf) {
    BOOL check = TRUE;
#if ENABLE_AMP
    if (!conf->enc.use_auto_npass)
        return check;
    if (conf->vid.amp_check & AMPLIMIT_BITRATE_UPPER) {
        //if (conf->x264.bitrate > conf->vid.amp_limit_bitrate_upper) {
        //    check = FALSE; error_amp_bitrate_confliction();
        //} else if (conf->vid.amp_limit_bitrate_upper <= 0.0)
        //    conf->vid.amp_check &= ~AMPLIMIT_BITRATE; //フラグを折る
        if (conf->vid.amp_limit_bitrate_upper <= 0.0)
            conf->vid.amp_check &= ~AMPLIMIT_BITRATE_UPPER; //フラグを折る
    }
    if (conf->vid.amp_check & AMPLIMIT_FILE_SIZE) {
        if (conf->vid.amp_limit_file_size <= 0.0)
            conf->vid.amp_check &= ~AMPLIMIT_FILE_SIZE; //フラグを折る
    }
    if (conf->vid.amp_check && conf->vid.afs && AUDIO_DELAY_CUT_ADD_VIDEO == conf->aud.delay_cut) {
        check = FALSE; error_amp_afs_audio_delay_confliction();
    }
#endif
    return check;
}

static BOOL muxer_supports_audio_format(const int muxer_to_be_used, const AUDIO_SETTINGS *aud_stg) {
    switch (muxer_to_be_used) {
    case MUXER_TC2MP4:
    case MUXER_MP4_RAW:
    case MUXER_MP4:
        return aud_stg->unsupported_mp4 == 0;
    case MUXER_MKV:
    case MUXER_DISABLED:
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL check_if_exedit_is_used() {
    char name[256];
    wsprintf(name, "exedit_%d_%d", '01', GetCurrentProcessId());
    auto handle = unique_handle(OpenFileMapping(FILE_MAP_WRITE, FALSE, name),
        [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });

    return handle != nullptr;
}

static std::string find_auo_check_fileopen(const char *defaultExeDir, const char *defaultExeDir2) {
    char exe_path[MAX_PATH_LEN] = { 0 };
    PathCombine(exe_path, defaultExeDir, AUO_CHECK_FILEOPEN_NAME);
    if (PathFileExists(exe_path)) {
        return exe_path;
    }
    PathCombine(exe_path, defaultExeDir2, AUO_CHECK_FILEOPEN_NAME);
    if (PathFileExists(exe_path)) {
        return exe_path;
    }
    return "";
}

static BOOL check_temp_file_open(const char *target, const std::string& auo_check_fileopen_path, const bool check_dir, const bool auo_check_fileopen_warning) {
    DWORD err = ERROR_SUCCESS;

    if (is_64bit_os() && (auo_check_fileopen_path.length() == 0 || !PathFileExists(auo_check_fileopen_path.c_str())) && auo_check_fileopen_warning) {
        warning_no_auo_check_fileopen();
    }

    char test_filename[MAX_PATH_LEN];
    if (check_dir) {
        PathCombineLong(test_filename, _countof(test_filename), target, "auo_test_tempfile.tmp");
        avoid_exsisting_tmp_file(test_filename, _countof(test_filename));
    } else {
        strcpy_s(test_filename, target);
    }

    if (is_64bit_os() && auo_check_fileopen_path.length() > 0 && PathFileExists(auo_check_fileopen_path.c_str())) {
        //64bit OSでは、32bitアプリに対してはVirtualStoreが働く一方、
        //64bitアプリに対してはVirtualStoreが働かない
        //x264を64bitで実行することを考慮すると、
        //Aviutl(32bit)からチェックしても意味がないので、64bitプロセスからのチェックを行う
        PROCESS_INFORMATION pi;
        PIPE_SET pipes;
        InitPipes(&pipes);

        char fullargs[4096] = { 0 };
        sprintf_s(fullargs, "\"%s\" \"%s\"", auo_check_fileopen_path.c_str(), test_filename);

        char exeDir[MAX_PATH_LEN];
        strcpy_s(exeDir, auo_check_fileopen_path.c_str());
        PathRemoveFileSpecFixed(exeDir);

        int ret = 0;
        if ((ret = RunProcess(fullargs, exeDir, &pi, &pipes, NORMAL_PRIORITY_CLASS, TRUE, FALSE)) == RP_SUCCESS) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &err);
            CloseHandle(pi.hProcess);
        }
        if (err == ERROR_SUCCESS) {
            return TRUE;
        }
    } else {
        auto handle = unique_handle(CreateFile(test_filename, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL),
            [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
        if (handle.get() != INVALID_HANDLE_VALUE) {
            handle.reset();
            DeleteFile(test_filename);
            return TRUE;
        }
        err = GetLastError();
    }
    if (err != ERROR_ALREADY_EXISTS) {
        char *mesBuffer = nullptr;
        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&mesBuffer, 0, NULL);
        if (check_dir) {
            error_failed_to_open_tempdir(target, mesBuffer, err);
        } else {
            error_failed_to_open_tempfile(target, mesBuffer, err);
        }
        if (mesBuffer != nullptr) {
            LocalFree(mesBuffer);
        }
    }
    return FALSE;
}

BOOL audio_encoder_exe_exists(const CONF_GUIEX *conf, const guiEx_settings *exstg) {
    AUDIO_SETTINGS *aud_stg = &exstg->s_aud[conf->aud.encoder];
    if (!str_has_char(aud_stg->filename)) {
        return TRUE;
    }
    if (conf->aud.encoder == exstg->s_aud_faw_index) {
        return TRUE;
    }
    return PathFileExists(aud_stg->fullpath);
}

BOOL check_output(CONF_GUIEX *conf, OUTPUT_INFO *oip, const PRM_ENC *pe, guiEx_settings *exstg) {
    BOOL check = TRUE;
    //ファイル名長さ
    if (strlen(oip->savefile) > (MAX_PATH_LEN - MAX_APPENDIX_LEN - 1)) {
        error_filename_too_long();
        check = FALSE;
    }

    char aviutl_dir[MAX_PATH_LEN] = { 0 };
    get_aviutl_dir(aviutl_dir, _countof(aviutl_dir));

    char defaultExeDir[MAX_PATH_LEN] = { 0 };
    PathCombineLong(defaultExeDir, _countof(defaultExeDir), aviutl_dir, DEFAULT_EXE_DIR);

    char pluginsDir[MAX_PATH_LEN] = { 0 };
    char defaultExeDir2[MAX_PATH_LEN] = { 0 };
    get_auo_dir(pluginsDir, _countof(pluginsDir));
    PathCombineLong(defaultExeDir2, _countof(defaultExeDir2), pluginsDir, DEFAULT_EXE_DIR);

    const auto auo_check_fileopen_path = find_auo_check_fileopen(defaultExeDir, defaultExeDir2);

    //ダメ文字・環境依存文字チェック
    char savedir[MAX_PATH_LEN] = { 0 };
    strcpy_s(savedir, oip->savefile);
    PathRemoveFileSpecFixed(savedir);
    if (!PathIsDirectory(savedir)) {
        error_savdir_do_not_exist(oip->savefile, savedir);
        check = FALSE;
    //出力フォルダにファイルを開けるかどうか
    } else if (!check_temp_file_open(savedir, auo_check_fileopen_path, true, true)) {
        check = FALSE;
    //一時ファイルを開けるかどうか
    } else if (!check_temp_file_open(pe->temp_filename, auo_check_fileopen_path, false, false)) {
        check = FALSE;
    }

    if (check_file_is_aviutl_opened_file(oip->savefile, pe)) {
        error_file_is_already_opened_by_aviutl();
        check = FALSE;
    }

    //解像度
#if ENCODER_X264 || ENCODER_X265
    int w_mul = 1, h_mul = 1;
    switch (conf->enc.output_csp) {
        case OUT_CSP_YUV444:
        case OUT_CSP_RGB:
            w_mul = 1, h_mul = 1; break;
        case OUT_CSP_NV16:
        case OUT_CSP_YUV422:
            w_mul = 2, h_mul = 1; break;
        case OUT_CSP_NV12:
        case OUT_CSP_YV12:
        default:
            w_mul = 2; h_mul = 2; break;
    }
    if (conf->enc.interlaced) h_mul *= 2;
#else
    const int w_mul = 2, h_mul = 2;
#endif
    if (oip->w % w_mul) {
        error_invalid_resolution(TRUE,  w_mul, oip->w, oip->h);
        check = FALSE;
    }
    if (oip->h % h_mul) {
        //error_invalid_resolution(FALSE, h_mul, oip->w, oip->h);
        //check = FALSE;
        //切り捨て
        oip->h = (int)(oip->h / h_mul) * h_mul;
    }

    //出力するもの
    if (pe->video_out_type == VIDEO_OUTPUT_DISABLED && !(oip->flag & OUTPUT_INFO_FLAG_AUDIO)) {
        error_nothing_to_output();
        check = FALSE;
    }
    if (pe->video_out_type != VIDEO_OUTPUT_DISABLED && oip->n <= 0) {
        error_output_zero_frames();
        check = FALSE;
    }

    if (conf->oth.out_audio_only)
        write_log_auo_line(LOG_INFO, g_auo_mes.get(AUO_ENCODE_AUDIO_ONLY));

    const auto exeFiles = find_exe_files(defaultExeDir, defaultExeDir2);

    //必要な実行ファイル
    if (!conf->oth.disable_guicmd && pe->video_out_type != VIDEO_OUTPUT_DISABLED) {
        if (!PathFileExists(exstg->s_enc.fullpath)) {
            const auto targetExes = find_target_exe_files(ENCODER_APP_NAME, exeFiles);
            if (targetExes.size() > 0) {
                const auto latestVidEnc = find_latest_videnc(targetExes);
                if (exstg->s_local.get_relative_path) {
                    GetRelativePathTo(exstg->s_enc.fullpath, _countof(exstg->s_enc.fullpath), latestVidEnc.string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                } else {
                    strcpy_s(exstg->s_enc.fullpath, latestVidEnc.string().c_str());
                }
            }
            if (!PathFileExists(exstg->s_enc.fullpath)) {
                error_no_exe_file(ENCODER_APP_NAME_W, exstg->s_enc.fullpath);
                check = FALSE;
            }
        }
        info_use_exe_found(ENCODER_NAME_W, exstg->s_enc.fullpath);
    }

    //音声エンコーダ
    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        //音声長さチェック
        if (check_audio_length(oip, exstg->s_local.av_length_threshold)) {
            check = FALSE;
        }

        const bool default_audenc_cnf_avail = (exstg->s_local.default_audio_encoder < exstg->s_aud_count
            && str_has_char(exstg->s_aud[exstg->s_local.default_audio_encoder].filename));
        const bool default_audenc_auo_avail = (DEFAULT_AUDIO_ENCODER < exstg->s_aud_count
                && str_has_char(exstg->s_aud[DEFAULT_AUDIO_ENCODER].filename));
        if ((conf->aud.encoder < 0 || exstg->s_aud_count <= conf->aud.encoder)) {
            if (default_audenc_cnf_avail
                && 0 <= exstg->s_local.default_audio_encoder && exstg->s_local.default_audio_encoder < exstg->s_aud_count
                && muxer_supports_audio_format(pe->muxer_to_be_used, &exstg->s_aud[exstg->s_local.default_audio_encoder])) {
                conf->aud.encoder = exstg->s_local.default_audio_encoder;
                warning_use_default_audio_encoder(exstg->s_aud[conf->aud.encoder].dispname);
            } else if (default_audenc_auo_avail) {
                conf->aud.encoder = DEFAULT_AUDIO_ENCODER;
                warning_use_default_audio_encoder(exstg->s_aud[conf->aud.encoder].dispname);
            }
        }
        for (;;) {
            if (conf->aud.encoder < 0 || exstg->s_aud_count <= conf->aud.encoder) {
                error_invalid_ini_file();
                check = FALSE;
                break;
            }
            AUDIO_SETTINGS *aud_stg = &exstg->s_aud[conf->aud.encoder];
            if (!muxer_supports_audio_format(pe->muxer_to_be_used, aud_stg)) {
                const int orig_encoder = conf->aud.encoder;
                if (default_audenc_cnf_avail
                    && orig_encoder != exstg->s_local.default_audio_encoder
                    && 0 <= exstg->s_local.default_audio_encoder && exstg->s_local.default_audio_encoder < exstg->s_aud_count
                    && muxer_supports_audio_format(pe->muxer_to_be_used, &exstg->s_aud[exstg->s_local.default_audio_encoder])) {
                    conf->aud.encoder = exstg->s_local.default_audio_encoder;
                } else if (default_audenc_auo_avail) {
                    conf->aud.encoder = DEFAULT_AUDIO_ENCODER;
                }
                error_unsupported_audio_format_by_muxer(pe->video_out_type,
                    exstg->s_aud[orig_encoder].dispname,
                    (orig_encoder != conf->aud.encoder) ? exstg->s_aud[conf->aud.encoder].dispname : nullptr);
                // 同じエンコーダあるいはデフォルトエンコーダがうまく取得できな場合は再チェックしても意味がない
                if (orig_encoder == conf->aud.encoder) {
                    check = FALSE;
                    break;
                }
                // デフォルトエンコーダに戻して再チェック
                warning_use_default_audio_encoder(exstg->s_aud[conf->aud.encoder].dispname);
                continue;
            }
            if (!audio_encoder_exe_exists(conf, exstg)) {
                //とりあえず、exe_filesを探す
                {
                    const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                    if (targetExes.size() > 0) {
                        if (exstg->s_local.get_relative_path) {
                            GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                        } else {
                            strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                        }
                    }
                }
                //みつからなければ、デフォルトエンコーダを探す
                if (!PathFileExists(aud_stg->fullpath) && default_audenc_cnf_avail
                    && 0 <= exstg->s_local.default_audio_encoder && exstg->s_local.default_audio_encoder < exstg->s_aud_count
                    && muxer_supports_audio_format(pe->muxer_to_be_used, &exstg->s_aud[exstg->s_local.default_audio_encoder])) {
                    conf->aud.encoder = exstg->s_local.default_audio_encoder;
                    aud_stg = &exstg->s_aud[conf->aud.encoder];
                    if (!PathFileExists(aud_stg->fullpath)) {
                        const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                        if (targetExes.size() > 0) {
                            if (exstg->s_local.get_relative_path) {
                                GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                            } else {
                                strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                            }
                            warning_use_default_audio_encoder(aud_stg->dispname);
                        }
                    }
                }
                if (!PathFileExists(aud_stg->fullpath) && default_audenc_auo_avail) {
                    conf->aud.encoder = DEFAULT_AUDIO_ENCODER;
                    aud_stg = &exstg->s_aud[conf->aud.encoder];
                    if (!PathFileExists(aud_stg->fullpath)) {
                        const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                        if (targetExes.size() > 0) {
                            if (exstg->s_local.get_relative_path) {
                                GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                            } else {
                                strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                            }
                            warning_use_default_audio_encoder(aud_stg->dispname);
                        }
                    }
                }
                if (!PathFileExists(aud_stg->fullpath)) {
                    //fawの場合はOK
                    if (conf->aud.encoder != exstg->s_aud_faw_index) {
                        error_no_exe_file(aud_stg->dispname, aud_stg->fullpath);
                        check = FALSE;
                        break;
                    }
                }
            }
            if (str_has_char(aud_stg->filename) && (conf->aud.encoder != exstg->s_aud_faw_index)) {
                std::wstring exe_message;
                if (!check_audenc_output(aud_stg, exe_message)) {
                    const int orig_encoder = conf->aud.encoder;
                    if (default_audenc_cnf_avail
                        && orig_encoder != exstg->s_local.default_audio_encoder
                        && 0 <= exstg->s_local.default_audio_encoder && exstg->s_local.default_audio_encoder < exstg->s_aud_count
                        && muxer_supports_audio_format(pe->muxer_to_be_used, &exstg->s_aud[exstg->s_local.default_audio_encoder])) {
                        conf->aud.encoder = exstg->s_local.default_audio_encoder;
                    } else if (default_audenc_auo_avail) {
                        conf->aud.encoder = DEFAULT_AUDIO_ENCODER;
                    }
                    error_failed_to_run_audio_encoder(
                        exstg->s_aud[orig_encoder].dispname,
                        exe_message.c_str(),
                        (orig_encoder != conf->aud.encoder) ? exstg->s_aud[conf->aud.encoder].dispname : nullptr);
                    // 同じエンコーダあるいはデフォルトエンコーダがうまく取得できな場合は再チェックしても意味がない
                    if (orig_encoder == conf->aud.encoder) {
                        check = FALSE;
                        break;
                    }
                    // デフォルトエンコーダに戻して再チェック
                    warning_use_default_audio_encoder(exstg->s_aud[conf->aud.encoder].dispname);
                    continue;
                }
                info_use_exe_found(aud_stg->dispname, aud_stg->fullpath);
            }
            // ここまで来たらエンコーダの確認終了なのでbreak
            break;
        }
    }

    //muxer
    switch (pe->muxer_to_be_used) {
        case MUXER_TC2MP4:
            check &= check_muxer_exist(&exstg->s_mux[MUXER_TC2MP4], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            //下へフォールスルー
        case MUXER_MP4:
            check &= check_muxer_exist(&exstg->s_mux[MUXER_MP4], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            if (str_has_char(exstg->s_mux[MUXER_MP4_RAW].base_cmd)) {
                check &= check_muxer_exist(&exstg->s_mux[MUXER_MP4_RAW], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            }
            check &= check_muxer_matched_with_ini(exstg->s_mux);
            break;
        case MUXER_MKV:
            check &= check_muxer_exist(&exstg->s_mux[pe->muxer_to_be_used], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            break;
        default:
            break;
    }

    //自動マルチパス設定
    check &= check_amp(conf);

    //オーディオディレイカット
    if (conf->vid.afs && AUDIO_DELAY_CUT_ADD_VIDEO == conf->aud.delay_cut) {
        info_afs_audio_delay_confliction();
        conf->aud.audio_encode_timing = 0;
    }

    return check;
}

void open_log_window(const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat, int current_pass, int total_pass, bool amp_crf_reenc) {
    wchar_t mes[MAX_PATH_LEN + 512];
    const wchar_t *newLine = (get_current_log_len(current_pass == 1 && !amp_crf_reenc)) ? L"\r\n\r\n" : L""; //必要なら行送り
    static const wchar_t *SEPARATOR = L"------------------------------------------------------------------------------------------------------------------------------";
    const std::wstring savefile_w = char_to_wstring(oip->savefile);
    if (total_pass < 2 || current_pass > total_pass)
        swprintf_s(mes, L"%s%s\r\n[%s]\r\n%s", newLine, SEPARATOR, savefile_w.c_str(), SEPARATOR);
    else
        swprintf_s(mes, L"%s%s\r\n[%s] (%d / %d pass)\r\n%s", newLine, SEPARATOR, savefile_w.c_str(), current_pass, total_pass, SEPARATOR);

    show_log_window(sys_dat->aviutl_dir, sys_dat->exstg->s_local.disable_visual_styles);
    write_log_line(LOG_INFO, mes);

    char cpu_info[256];
    getCPUInfo(cpu_info);
    DWORD buildNumber = 0;
    const auto osver = char_to_wstring(getOSVersion(&buildNumber));
    write_log_auo_line_fmt(LOG_INFO, L"%s %s / %s %s (%d) / %s",
        AUO_NAME_WITHOUT_EXT_W, AUO_VERSION_STR_W, osver.c_str(), is_64bit_os() ? L"x64" : L"x86", buildNumber, char_to_wstring(cpu_info).c_str());

    if (oip->flag & OUTPUT_INFO_FLAG_VIDEO) {
        const double video_length = oip->n * (double)oip->scale / oip->rate;

        const int vid_h = (int)(video_length / 3600);
        const int vid_m = (int)(video_length - vid_h * 3600) / 60;
        const int vid_s = (int)(video_length - vid_h * 3600 - vid_m * 60);
        const int vid_ms = std::min((int)((video_length - (double)(vid_h * 3600 + vid_m * 60 + vid_s)) * 1000.0), 999);

        write_log_auo_line_fmt(LOG_INFO, L"video: %d:%02d:%02d.%03d %d/%d(%.3f) fps",
            vid_h, vid_m, vid_s, vid_ms, oip->rate, oip->scale, oip->rate / (double)oip->scale);
    }

    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        const double audio_length = oip->audio_n / (double)oip->audio_rate;

        const int aud_h = (int)audio_length / 3600;
        const int aud_m = (int)(audio_length - aud_h * 3600) / 60;
        const int aud_s = (int)(audio_length - aud_h * 3600 - aud_m * 60);
        const int aud_ms = std::min((int)((audio_length - (double)(aud_h * 3600 + aud_m * 60 + aud_s)) * 1000.0), 999);

        write_log_auo_line_fmt(LOG_INFO, L"audio: %d:%02d:%02d.%03d %dch %.1fkHz %d samples",
            aud_h, aud_m, aud_s, aud_ms, oip->audio_ch, oip->audio_rate / 1000.0, oip->audio_n);
    }
}

static void set_tmpdir(PRM_ENC *pe, int tmp_dir_index, const char *savefile, const SYSTEM_DATA *sys_dat) {
    if (tmp_dir_index < TMP_DIR_OUTPUT || TMP_DIR_CUSTOM < tmp_dir_index)
        tmp_dir_index = TMP_DIR_OUTPUT;

    if (tmp_dir_index == TMP_DIR_SYSTEM) {
        //システムの一時フォルダを取得
        if (GetTempPath(_countof(pe->temp_filename), pe->temp_filename) != NULL) {
            PathRemoveBackslash(pe->temp_filename);
            write_log_auo_line_fmt(LOG_INFO, L"%s : %s", g_auo_mes.get(AUO_ENCODE_TMP_FOLDER), char_to_wstring(pe->temp_filename).c_str());
        } else {
            warning_failed_getting_temp_path();
            tmp_dir_index = TMP_DIR_OUTPUT;
        }
    }
    if (tmp_dir_index == TMP_DIR_CUSTOM) {
        //指定されたフォルダ
        if (DirectoryExistsOrCreate(sys_dat->exstg->s_local.custom_tmp_dir)) {
            strcpy_s(pe->temp_filename, GetFullPathFrom(sys_dat->exstg->s_local.custom_tmp_dir, sys_dat->aviutl_dir).c_str());
            PathRemoveBackslash(pe->temp_filename);

            //指定された一時フォルダにファイルを作成できるか確認する
            char defaultExeDir[MAX_PATH_LEN] = { 0 };
            PathCombineLong(defaultExeDir, _countof(defaultExeDir), sys_dat->aviutl_dir, DEFAULT_EXE_DIR);

            char pluginsDir[MAX_PATH_LEN] = { 0 };
            char defaultExeDir2[MAX_PATH_LEN] = { 0 };
            get_auo_dir(pluginsDir, _countof(pluginsDir));
            PathCombineLong(defaultExeDir2, _countof(defaultExeDir2), pluginsDir, DEFAULT_EXE_DIR);

            const auto auo_check_fileopen_path = find_auo_check_fileopen(defaultExeDir, defaultExeDir2);

            if (check_temp_file_open(pe->temp_filename, auo_check_fileopen_path, true, false)) {
                write_log_auo_line_fmt(LOG_INFO, L"%s : %s", g_auo_mes.get(AUO_ENCODE_TMP_FOLDER), char_to_wstring(pe->temp_filename).c_str());
            } else {
                warning_unable_to_open_tempfile(sys_dat->exstg->s_local.custom_tmp_dir);
                tmp_dir_index = TMP_DIR_OUTPUT;
            }
        } else {
            warning_no_temp_root(sys_dat->exstg->s_local.custom_tmp_dir);
            tmp_dir_index = TMP_DIR_OUTPUT;
        }
    }
    if (tmp_dir_index == TMP_DIR_OUTPUT) {
        //出力フォルダと同じ("\"なし)
        strcpy_s(pe->temp_filename, _countof(pe->temp_filename), savefile);
        PathRemoveFileSpecFixed(pe->temp_filename);
    }
}

static void set_aud_delay_cut(CONF_GUIEX *conf, PRM_ENC *pe, const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat) {
    pe->delay_cut_additional_vframe = 0;
    pe->delay_cut_additional_aframe = 0;
    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        int audio_delay = sys_dat->exstg->s_aud[conf->aud.encoder].mode[conf->aud.enc_mode].delay;
        if (audio_delay) {
            const double fps = oip->rate / (double)oip->scale;
            const int audio_rate = oip->audio_rate;
            switch (conf->aud.delay_cut) {
            case AUDIO_DELAY_CUT_DELETE_AUDIO:
                pe->delay_cut_additional_aframe = -1 * audio_delay;
                break;
            case AUDIO_DELAY_CUT_ADD_VIDEO:
                pe->delay_cut_additional_vframe = additional_vframe_for_aud_delay_cut(fps, audio_rate, audio_delay);
                pe->delay_cut_additional_aframe = additional_silence_for_aud_delay_cut(fps, audio_rate, audio_delay);
                break;
            case AUDIO_DELAY_CUT_NONE:
            case AUDIO_DELAY_CUT_EDTS:
            default:
                break;
            }
        } else {
            conf->aud.delay_cut = AUDIO_DELAY_CUT_NONE;
        }
    }
}

bool use_auto_npass(const CONF_GUIEX *conf) {
#if ENCODER_SVTAV1	
    if (!conf->oth.disable_guicmd) {
        CONF_ENCODER enc = get_default_prm();
        set_cmd(&enc, conf->enc.cmd, true);
        return enc.pass > 1;
    }
    return false;
#else
    return conf->enc.use_auto_npass;
#endif    
}

int get_total_path(const CONF_GUIEX *conf) {
#if ENCODER_SVTAV1
    return use_auto_npass(conf) ? 2 : 1;
#else
    return (conf->enc.use_auto_npass
         && conf->enc.rc_mode == X264_RC_BITRATE
         && !conf->oth.disable_guicmd)
         ? conf->enc.auto_npass : 1;
#endif
}

void free_enc_prm(PRM_ENC *pe) {
    if (pe->opened_aviutl_files) {
        for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
            if (pe->opened_aviutl_files[i]) {
                free(pe->opened_aviutl_files[i]);
            }
        }
        free(pe->opened_aviutl_files);
        pe->opened_aviutl_files = nullptr;
        pe->n_opened_aviutl_files = 0;
    }
}

void init_enc_prm(const CONF_GUIEX *conf, PRM_ENC *pe, OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat) {
    //初期化
    ZeroMemory(pe, sizeof(PRM_ENC));
    //設定更新
    sys_dat->exstg->load_encode_stg();
    sys_dat->exstg->load_append();
    sys_dat->exstg->load_fn_replace();

    strcpy_s(pe->save_file_name, oip->savefile);
    pe->video_out_type = check_video_ouput(conf, oip);

    // 不明な拡張子だった場合、デフォルトの出力拡張子を付与する
    if (pe->video_out_type == VIDEO_OUTPUT_UNKNOWN) {
        int out_ext_idx = sys_dat->exstg->s_local.default_output_ext;
        if (out_ext_idx < 0 || out_ext_idx >= _countof(OUTPUT_FILE_EXT)) {
            out_ext_idx = 0;
        }
        // 拡張子を付与
        strcat_s(pe->save_file_name, OUTPUT_FILE_EXT[out_ext_idx]);
        // ファイル名が重複していた場合、連番を付与する
        if (PathFileExists(pe->save_file_name)) {
            char tmp[MAX_PATH_LEN];
            for (int i = 0; i < 1000000; i++) {
                char new_ext[32];
                sprintf_s(new_ext, ".%d%s", i, OUTPUT_FILE_EXT[out_ext_idx]);
                strcpy_s(tmp, pe->save_file_name);
                change_ext(tmp, _countof(tmp), new_ext);
                if (!PathFileExists(tmp)) {
                    strcpy_s(pe->save_file_name, tmp);
                    break;
                }
            }
        }
        // オリジナルのsavefileのポインタを保存
        pe->org_save_file_name = oip->savefile;
        // 保存先のファイル名を変更
        oip->savefile = pe->save_file_name;
        // 再度チェック
        pe->video_out_type = check_video_ouput(conf, oip);
    }
}

void set_enc_prm(CONF_GUIEX *conf, PRM_ENC *pe, const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat) {
    pe->video_out_type = check_video_ouput(conf, oip);
    pe->total_pass = get_total_path(conf);
    pe->amp_pass_limit = pe->total_pass + sys_dat->exstg->s_local.amp_retry_limit;
    pe->amp_reset_pass_count = 0;
    pe->amp_reset_pass_limit = sys_dat->exstg->s_local.amp_retry_limit;
    pe->current_pass = 1;
    pe->drop_count = 0;
    memcpy(&pe->append, &sys_dat->exstg->s_append, sizeof(FILE_APPENDIX));
    ZeroMemory(&pe->append.aud, sizeof(pe->append.aud));
    create_aviutl_opened_file_list(pe);

    //一時フォルダの決定
    set_tmpdir(pe, conf->oth.temp_dir, oip->savefile, sys_dat);

    //音声一時フォルダの決定
    char *cus_aud_tdir = pe->temp_filename;
    if (conf->aud.aud_temp_dir) {
        if (DirectoryExistsOrCreate(sys_dat->exstg->s_local.custom_audio_tmp_dir)) {
            cus_aud_tdir = sys_dat->exstg->s_local.custom_audio_tmp_dir;
            write_log_auo_line_fmt(LOG_INFO, L"%s : %s", g_auo_mes.get(AUO_ENCODE_TMP_FOLDER_AUDIO), char_to_wstring(GetFullPathFrom(cus_aud_tdir, sys_dat->aviutl_dir)).c_str());
        } else {
            warning_no_aud_temp_root(sys_dat->exstg->s_local.custom_audio_tmp_dir);
        }
    }
    strcpy_s(pe->aud_temp_dir, GetFullPathFrom(cus_aud_tdir, sys_dat->aviutl_dir).c_str());

    //ファイル名置換を行い、一時ファイル名を作成
    char filename_replace[MAX_PATH_LEN] = { 0 };
    strcpy_s(filename_replace, _countof(filename_replace), PathFindFileName(oip->savefile));
    sys_dat->exstg->apply_fn_replace(filename_replace, _countof(filename_replace));
    PathCombineLong(pe->temp_filename, _countof(pe->temp_filename), pe->temp_filename, filename_replace);

    if (pe->video_out_type != VIDEO_OUTPUT_DISABLED) {
        if (!check_videnc_mp4_output(sys_dat->exstg->s_enc.fullpath, pe->temp_filename)) {
            //一時ファイルの拡張子を変更
            change_ext(pe->temp_filename, _countof(pe->temp_filename), ENOCDER_RAW_EXT);
            if (ENCODER_X264) warning_x264_mp4_output_not_supported();
        }
    }
    //ファイルの上書きを避ける
    avoid_exsisting_tmp_file(pe->temp_filename, _countof(pe->temp_filename));

    pe->muxer_to_be_used = check_muxer_to_be_used(conf, sys_dat, pe->temp_filename, pe->video_out_type, (oip->flag & OUTPUT_INFO_FLAG_AUDIO) != 0);
    if (pe->muxer_to_be_used >= 0) {
        const MUXER_CMD_EX *muxer_mode = &sys_dat->exstg->s_mux[pe->muxer_to_be_used].ex_cmd[get_mux_excmd_mode(conf, pe)];
        if (str_has_char(muxer_mode->chap_file) && strstr(muxer_mode->chap_file, "chapter.%{pid}.txt")) {
            char move_to[MAX_PATH_LEN] = { 0 };
            char move_from[MAX_PATH_LEN] = { 0 };
            strcpy_s(move_to, muxer_mode->chap_file);
            strcpy_s(move_from, muxer_mode->chap_file);
            replace(move_from, sizeof(move_from), "%{pid}.", "");
            cmd_replace(move_to, sizeof(move_to), pe, sys_dat, conf, oip);
            cmd_replace(move_from, sizeof(move_from), pe, sys_dat, conf, oip);
            if (PathFileExists(move_from)) {
                if (PathFileExists(move_to))
                    remove(move_to);
                if (rename(move_from, move_to))
                    write_log_auo_line(LOG_WARNING, g_auo_mes.get(AUO_ENCODE_ERROR_MOVE_CHAPTER_FILE));
            }
        }
    }

    //FAWチェックとオーディオディレイの修正
    if (conf->aud.faw_check)
        auo_faw_check(&conf->aud, oip, pe, sys_dat->exstg);
    set_aud_delay_cut(conf, pe, oip, sys_dat);
}

void auto_save_log(const CONF_GUIEX *conf, const OUTPUT_INFO *oip, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const bool force_save) {
    guiEx_settings ex_stg(true);
    ex_stg.load_log_win();
    if (!force_save && !ex_stg.s_log.auto_save_log)
        return;
    char log_file_path[MAX_PATH_LEN];
    if (AUO_RESULT_SUCCESS != getLogFilePath(log_file_path, _countof(log_file_path), pe, sys_dat, conf, oip))
        warning_no_auto_save_log_dir();
    auto_save_log_file(log_file_path);
    return;
}

void warn_video_length(const OUTPUT_INFO *oip) {
    const double fps = oip->rate / (double)oip->scale;
    if (oip->n <= (int)(fps + 0.5)) {
        warning_video_very_short();
    }
}

int additional_vframe_for_aud_delay_cut(double fps, int audio_rate, int audio_delay) {
    double delay_sec = audio_delay / (double)audio_rate;
    return (int)ceil(delay_sec * fps);
}

int additional_silence_for_aud_delay_cut(double fps, int audio_rate, int audio_delay, int vframe_added) {
    vframe_added = (vframe_added >= 0) ? vframe_added : additional_vframe_for_aud_delay_cut(fps, audio_rate, audio_delay);
    return (int)(vframe_added / (double)fps * audio_rate + 0.5) - audio_delay;
}

BOOL fps_after_afs_is_24fps(const int frame_n, const PRM_ENC *pe) {
    return (pe->drop_count > (frame_n * 0.10));
}

int get_mux_excmd_mode(const CONF_GUIEX *conf, const PRM_ENC *pe) {
    int mode = 0;
    switch (pe->muxer_to_be_used) {
        case MUXER_MKV:     mode = conf->mux.mkv_mode; break;
        case MUXER_MP4:
        case MUXER_TC2MP4:
        case MUXER_MP4_RAW: mode = conf->mux.mp4_mode; break;
    }
    return mode;
}

void get_aud_filename(char *audfile, size_t nSize, const PRM_ENC *pe, int i_aud) {
    PathCombineLong(audfile, nSize, pe->aud_temp_dir, PathFindFileName(pe->temp_filename));
    apply_appendix(audfile, nSize, audfile, pe->append.aud[i_aud]);
}

static void get_muxout_appendix(char *muxout_appendix, size_t nSize, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe) {
    static const char * const MUXOUT_APPENDIX = "_muxout";
    strcpy_s(muxout_appendix, nSize, MUXOUT_APPENDIX);
    const char *ext = (pe->muxer_to_be_used >= 0 && str_has_char(sys_dat->exstg->s_mux[pe->muxer_to_be_used].out_ext)) ?
        sys_dat->exstg->s_mux[pe->muxer_to_be_used].out_ext : PathFindExtension(pe->temp_filename);
    strcat_s(muxout_appendix, nSize, ext);
}

void get_muxout_filename(char *filename, size_t nSize, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe) {
    char muxout_appendix[MAX_APPENDIX_LEN];
    get_muxout_appendix(muxout_appendix, sizeof(muxout_appendix), sys_dat, pe);
    apply_appendix(filename, nSize, pe->temp_filename, muxout_appendix);
}

//チャプターファイル名とapple形式のチャプターファイル名を同時に作成する
void set_chap_filename(char *chap_file, size_t cf_nSize, char *chap_apple, size_t ca_nSize, const char *chap_base,
                       const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    strcpy_s(chap_file, cf_nSize, chap_base);
    cmd_replace(chap_file, cf_nSize, pe, sys_dat, conf, oip);
    apply_appendix(chap_apple, ca_nSize, chap_file, pe->append.chap_apple);
    sys_dat->exstg->apply_fn_replace(PathFindFileName(chap_apple), ca_nSize - (PathFindFileName(chap_apple) - chap_apple));
}

void insert_num_to_replace_key(char *key, size_t nSize, int num) {
    char tmp[128];
    int key_len = strlen(key);
    sprintf_s(tmp, _countof(tmp), "%d%s", num, &key[key_len-1]);
    key[key_len-1] = '\0';
    strcat_s(key, nSize, tmp);
}

static void replace_aspect_ratio(char *cmd, size_t nSize, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    const int w = oip->w;
    const int h = oip->h;

#if ENCODER_X264 || ENCODER_X265
    int sar_x = conf->enc.sar.x;
    int sar_y = conf->enc.sar.y;
    int dar_x = 0;
    int dar_y = 0;
    if (sar_x * sar_y > 0) {
        if (sar_x < 0) {
            dar_x = -1 * sar_x;
            dar_y = -1 * sar_y;
            set_guiEx_auto_sar(&sar_x, &sar_y, w, h);
        } else {
            dar_x = sar_x * w;
            dar_y = sar_y * h;
            const int gcd = get_gcd(dar_x, dar_y);
            dar_x /= gcd;
            dar_y /= gcd;
        }
    }
    if (sar_x * sar_y <= 0)
        sar_x = sar_y = 1;
    if (dar_x * dar_y <= 0)
        dar_x = dar_y = 1;

    char buf[32];
    //%{sar_x} / %{par_x}
    sprintf_s(buf, _countof(buf), "%d", sar_x);
    replace(cmd, nSize, "%{sar_x}", buf);
    replace(cmd, nSize, "%{par_x}", buf);
    //%{sar_x} / %{sar_y}
    sprintf_s(buf, _countof(buf), "%d", sar_y);
    replace(cmd, nSize, "%{sar_y}", buf);
    replace(cmd, nSize, "%{par_y}", buf);
    //%{dar_x}
    sprintf_s(buf, _countof(buf), "%d", dar_x);
    replace(cmd, nSize, "%{dar_x}", buf);
    //%{dar_y}
    sprintf_s(buf, _countof(buf), "%d", dar_y);
    replace(cmd, nSize, "%{dar_y}", buf);
#elif ENCODER_SVTAV1
    int sar_x = conf->vid.sar_x;
    int sar_y = conf->vid.sar_y;
    int dar_x = 0;
    int dar_y = 0;
    if (sar_x * sar_y > 0) {
        if (sar_x < 0) {
            dar_x = -1 * sar_x;
            dar_y = -1 * sar_y;
            set_guiEx_auto_sar(&sar_x, &sar_y, w, h);
        } else {
            dar_x = sar_x * w;
            dar_y = sar_y * h;
            const int gcd = get_gcd(dar_x, dar_y);
            dar_x /= gcd;
            dar_y /= gcd;
        }
        if (sar_x * sar_y <= 0)
            sar_x = sar_y = 1;

        char buf[32];
        //%{sar_x} / %{par_x}
        sprintf_s(buf, _countof(buf), "%d", sar_x);
        replace(cmd, nSize, "%{sar_x}", buf);
        replace(cmd, nSize, "%{par_x}", buf);
        //%{sar_x} / %{sar_y}
        sprintf_s(buf, _countof(buf), "%d", sar_y);
        replace(cmd, nSize, "%{sar_y}", buf);
        replace(cmd, nSize, "%{par_y}", buf);
        if ((sar_x == 1 && sar_y == 1) || (dar_x * dar_y <= 0)) {
            del_arg(cmd, "%{dar_x}", -1);
            del_arg(cmd, "%{dar_y}", -1);
        } else {
            //%{dar_x}
            sprintf_s(buf, _countof(buf), "%d", dar_x);
            replace(cmd, nSize, "%{dar_x}", buf);
            //%{dar_y}
            sprintf_s(buf, _countof(buf), "%d", dar_y);
            replace(cmd, nSize, "%{dar_y}", buf);
        }
    } else {
        del_arg(cmd, "%{sar_x}", -1);
        del_arg(cmd, "%{sar_y}", -1);
        del_arg(cmd, "%{par_x}", -1);
        del_arg(cmd, "%{par_y}", -1);
        del_arg(cmd, "%{dar_x}", -1);
        del_arg(cmd, "%{dar_y}", -1);
    }
#else
    static_assert(false);
#endif
}

void cmd_replace(char *cmd, size_t nSize, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    char tmp[MAX_PATH_LEN] = { 0 };
    //置換操作の実行
    //%{vidpath}
    replace(cmd, nSize, "%{vidpath}", pe->temp_filename);
    //%{audpath}
    for (int i_aud = 0; i_aud < pe->aud_count; i_aud++) {
        if (str_has_char(pe->append.aud[i_aud])) {
            get_aud_filename(tmp, _countof(tmp), pe, i_aud);
            char aud_key[128] = "%{audpath}";
            if (i_aud)
                insert_num_to_replace_key(aud_key, _countof(aud_key), i_aud);
            replace(cmd, nSize, aud_key, tmp);
        }
    }
    //%{tmpdir}
    strcpy_s(tmp, _countof(tmp), pe->temp_filename);
    PathRemoveFileSpecFixed(tmp);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{tmpdir}", tmp);
    //%{tmpfile}
    strcpy_s(tmp, _countof(tmp), pe->temp_filename);
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{tmpfile}", tmp);
    //%{tmpname}
    strcpy_s(tmp, _countof(tmp), PathFindFileName(pe->temp_filename));
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{tmpname}", tmp);
    //%{savpath}
    replace(cmd, nSize, "%{savpath}", oip->savefile);
    //%{savfile}
    strcpy_s(tmp, _countof(tmp), oip->savefile);
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{savfile}", tmp);
    //%{savname}
    strcpy_s(tmp, _countof(tmp), PathFindFileName(oip->savefile));
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{savname}", tmp);
    //%{savdir}
    strcpy_s(tmp, _countof(tmp), oip->savefile);
    PathRemoveFileSpecFixed(tmp);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{savdir}", tmp);
    //%{aviutldir}
    strcpy_s(tmp, _countof(tmp), sys_dat->aviutl_dir);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{aviutldir}", tmp);
    //%{chpath}
    apply_appendix(tmp, _countof(tmp), oip->savefile, pe->append.chap);
    replace(cmd, nSize, "%{chpath}", tmp);
    //%{tcpath}
    apply_appendix(tmp, _countof(tmp), pe->temp_filename, pe->append.tc);
    replace(cmd, nSize, "%{tcpath}", tmp);
    //%{muxout}
    get_muxout_filename(tmp, _countof(tmp), sys_dat, pe);
    replace(cmd, nSize, "%{muxout}", tmp);
    //%{fps_rate}
    int fps_rate = oip->rate;
    int fps_scale = oip->scale;
#ifdef MSDK_SAMPLE_VERSION
    if (conf->qsv.vpp.nDeinterlace == MFX_DEINTERLACE_IT)
        fps_rate = (fps_rate * 4) / 5;
#endif
    const int fps_gcd = get_gcd(fps_rate, fps_scale);
    fps_rate /= fps_gcd;
    fps_scale /= fps_gcd;
    sprintf_s(tmp, sizeof(tmp), "%d", fps_rate);
    replace(cmd, nSize, "%{fps_rate}", tmp);
    //%{fps_rate_times_4}
    fps_rate *= 4;
    sprintf_s(tmp, sizeof(tmp), "%d", fps_rate);
    replace(cmd, nSize, "%{fps_rate_times_4}", tmp);
    //%{fps_scale}
    sprintf_s(tmp, sizeof(tmp), "%d", fps_scale);
    replace(cmd, nSize, "%{fps_scale}", tmp);
    //アスペクト比
    replace_aspect_ratio(cmd, nSize, conf, oip);
    //%{pid}
    sprintf_s(tmp, sizeof(tmp), "%d", GetCurrentProcessId());
    replace(cmd, nSize, "%{pid}", tmp);

    replace(cmd, nSize, ENCODER_REPLACE_MACRO, GetFullPathFrom(sys_dat->exstg->s_enc.fullpath,                    sys_dat->aviutl_dir).c_str());
    replace(cmd, nSize, "%{audencpath}",       GetFullPathFrom(sys_dat->exstg->s_aud[conf->aud.encoder].fullpath, sys_dat->aviutl_dir).c_str());
    replace(cmd, nSize, "%{mp4muxerpath}",     GetFullPathFrom(sys_dat->exstg->s_mux[MUXER_MP4].fullpath,         sys_dat->aviutl_dir).c_str());
    replace(cmd, nSize, "%{mkvmuxerpath}",     GetFullPathFrom(sys_dat->exstg->s_mux[MUXER_MKV].fullpath,         sys_dat->aviutl_dir).c_str());
}

static void remove_file(const char *target, const wchar_t *name) {
    if (!DeleteFile(target)) {
        auto errstr = getLastErrorStr(GetLastError());
        write_log_auo_line_fmt(LOG_WARNING, L"%s%s: %s (\"%s\")", name, g_auo_mes.get(AUO_ENCODE_FILE_REMOVE_FAILED), errstr.c_str(), char_to_wstring(target).c_str());
    }
}

static void move_file(const char *move_from, const char *move_to, const wchar_t *name) {
    if (!MoveFile(move_from, move_to)) {
        auto errstr = getLastErrorStr(GetLastError());
        write_log_auo_line_fmt(LOG_WARNING, L"%s%s: %s (\"%s\")", name, g_auo_mes.get(AUO_ENCODE_FILE_MOVE_FAILED), errstr.c_str(), char_to_wstring(move_to).c_str());
    }
}

//一時ファイルの移動・削除を行う
// move_from -> move_to
// temp_filename … 動画ファイルの一時ファイル名。これにappendixをつけてmove_from を作る。
//                  appndixがNULLのときはこれをそのままmove_fromとみなす。
// appendix      … ファイルの後修飾子。NULLも可。
// savefile      … 保存動画ファイル名。これにappendixをつけてmove_to を作る。NULLだと move_to に移動できない。
// ret, erase    … これまでのエラーと一時ファイルを削除するかどうか。エラーがない場合にのみ削除できる
// name          … 一時ファイルの種類の名前
// must_exist    … trueのとき、移動するべきファイルが存在しないとエラーを返し、ファイルが存在しないことを伝える
static BOOL move_temp_file(const char *appendix, const char *temp_filename, const char *savefile, DWORD ret, BOOL erase, const wchar_t *name, BOOL must_exist) {
    char move_from[MAX_PATH_LEN] = { 0 };
    if (appendix)
        apply_appendix(move_from, _countof(move_from), temp_filename, appendix);
    else
        strcpy_s(move_from, _countof(move_from), temp_filename);

    if (!PathFileExists(move_from)) {
        if (must_exist)
            write_log_auo_line_fmt(LOG_WARNING, L"%s%s", name, g_auo_mes.get(AUO_ENCODE_FILE_NOT_FOUND));
        return (must_exist) ? FALSE : TRUE;
    }
    if (ret == AUO_RESULT_SUCCESS && erase) {
        remove_file(move_from, name);
        return TRUE;
    }
    if (savefile == NULL || appendix == NULL)
        return TRUE;
    char move_to[MAX_PATH_LEN] = { 0 };
    apply_appendix(move_to, _countof(move_to), savefile, appendix);
    if (_stricmp(move_from, move_to) != NULL) {
        if (PathFileExists(move_to)) {
            remove_file(move_to, name);
        }
        move_file(move_from, move_to, name);
    }
    return TRUE;
}

AUO_RESULT move_temporary_files(const CONF_GUIEX *conf, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const OUTPUT_INFO *oip, DWORD ret) {
    //動画ファイル
    if (!conf->oth.out_audio_only) {
        if (!move_temp_file(PathFindExtension((pe->muxer_to_be_used >= 0) ? oip->savefile : pe->temp_filename), pe->temp_filename, oip->savefile, ret, FALSE, L"出力", !ret)) {
            ret |= AUO_RESULT_ERROR;
        }
    }
    //動画のみファイル
    if (str_has_char(pe->muxed_vid_filename) && PathFileExists(pe->muxed_vid_filename))
        remove_file(pe->muxed_vid_filename, L"映像一時ファイル");
    //mux後ファイル
    if (pe->muxer_to_be_used >= 0) {
        char muxout_appendix[MAX_APPENDIX_LEN];
        get_muxout_appendix(muxout_appendix, _countof(muxout_appendix), sys_dat, pe);
        move_temp_file(muxout_appendix, pe->temp_filename, oip->savefile, ret, FALSE, g_auo_mes.get(AUO_ENCODE_AFTER_MUX), FALSE);
    }
    //qpファイル
    move_temp_file(pe->append.qp,   pe->temp_filename, oip->savefile, ret, !sys_dat->exstg->s_local.keep_qp_file, L"qp", FALSE);
    //tcファイル
    BOOL erase_tc = is_afsvfr(conf) && !conf->vid.auo_tcfile_out && pe->muxer_to_be_used != MUXER_DISABLED;
    move_temp_file(pe->append.tc,   pe->temp_filename, oip->savefile, ret, erase_tc, g_auo_mes.get(AUO_ENCODE_TC_FILE), FALSE);
    //チャプターファイル
    if (pe->muxer_to_be_used >= 0) {
        const MUXER_CMD_EX *muxer_mode = &sys_dat->exstg->s_mux[pe->muxer_to_be_used].ex_cmd[get_mux_excmd_mode(conf, pe)];
        bool chapter_auf = strstr(muxer_mode->chap_file, "chapter.%{pid}.txt") != nullptr;
        if (sys_dat->exstg->s_local.auto_del_chap || chapter_auf) {
            char chap_file[MAX_PATH_LEN];
            char chap_apple[MAX_PATH_LEN];
            set_chap_filename(chap_file, _countof(chap_file), chap_apple, _countof(chap_apple), muxer_mode->chap_file, pe, sys_dat, conf, oip);
            move_temp_file(NULL, chap_file,  NULL, chapter_auf ? AUO_RESULT_SUCCESS : ret, TRUE, g_auo_mes.get(AUO_ENCODE_CHAPTER_FILE), FALSE);
            move_temp_file(NULL, chap_apple, NULL, chapter_auf ? AUO_RESULT_SUCCESS : ret, TRUE, g_auo_mes.get(AUO_ENCODE_CHAPTER_APPLE_FILE), FALSE);
        }
    }
    //ステータスファイル
    if (use_auto_npass(conf) && sys_dat->exstg->s_local.auto_del_stats) {
        char stats[MAX_PATH_LEN];
        strcpy_s(stats, sizeof(stats), conf->vid.stats);
        cmd_replace(stats, sizeof(stats), pe, sys_dat, conf, oip);
        move_temp_file(NULL, stats, NULL, ret, TRUE, g_auo_mes.get(AUO_ENCODE_STATUS_FILE), FALSE);
#if ENCODER_X264
        strcat_s(stats, sizeof(stats), ".mbtree");
        wchar_t mbtree_status[256];
        swprintf_s(mbtree_status, L"mbtree %s", g_auo_mes.get(AUO_ENCODE_STATUS_FILE));
        move_temp_file(NULL, stats, NULL, ret, TRUE, mbtree_status, FALSE);
#elif ENCODER_X265
        strcat_s(stats, sizeof(stats), ".cutree");
        wchar_t cutree_status[256];
        swprintf_s(cutree_status, L"cutree %s", g_auo_mes.get(AUO_ENCODE_STATUS_FILE));
        move_temp_file(NULL, stats, NULL, ret, TRUE, cutree_status, FALSE);
        if (conf->enc.analysis_reuse) {
            strcpy_s(stats, sizeof(stats), conf->vid.analysis_file);
            cmd_replace(stats, sizeof(stats), pe, sys_dat, conf, oip);
            move_temp_file(NULL, stats, NULL, ret, TRUE, L"analysis result", FALSE);
        }
#endif
    }
    //音声ファイル(wav)
    if (strcmp(pe->append.aud[0], pe->append.wav)) //「wav出力」ならここでは処理せず下のエンコード後ファイルとして扱う
        move_temp_file(pe->append.wav,  pe->temp_filename, oip->savefile, ret, TRUE, L"wav", FALSE);
    //音声ファイル(エンコード後ファイル)
    char aud_tempfile[MAX_PATH_LEN];
    PathCombineLong(aud_tempfile, _countof(aud_tempfile), pe->aud_temp_dir, PathFindFileName(pe->temp_filename));
    for (int i_aud = 0; i_aud < pe->aud_count; i_aud++)
        if (!move_temp_file(pe->append.aud[i_aud], aud_tempfile, oip->savefile, ret, !conf->oth.out_audio_only && pe->muxer_to_be_used != MUXER_DISABLED, g_auo_mes.get(AUO_ENCODE_AUDIO_FILE), conf->oth.out_audio_only))
            ret |= AUO_RESULT_ERROR;
    return ret;
}

DWORD GetExePriority(DWORD set, HANDLE h_aviutl) {
    if (set == AVIUTLSYNC_PRIORITY_CLASS)
        return (h_aviutl) ? GetPriorityClass(h_aviutl) : NORMAL_PRIORITY_CLASS;
    else
        return priority_table[set].value;
}

int check_video_ouput(const char *filename) {
    if (check_ext(filename, ".mp4"))  return VIDEO_OUTPUT_MP4;
    if (check_ext(filename, ".mkv"))  return VIDEO_OUTPUT_MKV;
    //if (check_ext(filename, ".mpg"))  return VIDEO_OUTPUT_MPEG2;
    //if (check_ext(filename, ".mpeg")) return VIDEO_OUTPUT_MPEG2;
    if (check_ext(filename, ENOCDER_RAW_EXT)) return VIDEO_OUTPUT_RAW;
    if (check_ext(filename, ".raw")) return VIDEO_OUTPUT_RAW;
    return VIDEO_OUTPUT_UNKNOWN;
}

int check_video_ouput(const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    if ((oip->flag & OUTPUT_INFO_FLAG_VIDEO) && !conf->oth.out_audio_only) {
        return check_video_ouput(oip->savefile);
    }
    return VIDEO_OUTPUT_DISABLED;
}

BOOL check_output_has_chapter(const CONF_GUIEX *conf, const SYSTEM_DATA *sys_dat, int muxer_to_be_used) {
    BOOL has_chapter = FALSE;
    if (muxer_to_be_used == MUXER_MKV || muxer_to_be_used == MUXER_TC2MP4 || muxer_to_be_used == MUXER_MP4) {
        const MUXER_CMD_EX *muxer_mode = &sys_dat->exstg->s_mux[muxer_to_be_used].ex_cmd[(muxer_to_be_used == MUXER_MKV) ? conf->mux.mkv_mode : conf->mux.mp4_mode];
        has_chapter = str_has_char(muxer_mode->chap_file);
    }
    return has_chapter;
}

BOOL check_tcfilein_is_used(const CONF_GUIEX *conf) {
#if ENABLE_TCFILE_IN
    return conf->enc.use_tcfilein || strstr(conf->vid.cmdex, "--tcfile-in") != nullptr;
#else
    return FALSE;
#endif
}

int check_muxer_to_be_used(const CONF_GUIEX *conf, const SYSTEM_DATA *sys_dat, const char *temp_filename, int video_output_type, BOOL audio_output) {
    //if (conf.vid.afs)
    //    conf.mux.disable_mp4ext = conf.mux.disable_mkvext = FALSE; //afsなら外部muxerを強制する

    int muxer_to_be_used = MUXER_DISABLED;
    if (video_output_type == VIDEO_OUTPUT_MP4 && !conf->mux.disable_mp4ext)
        muxer_to_be_used = is_afsvfr(conf) ? MUXER_TC2MP4 : MUXER_MP4;
    else if (video_output_type == VIDEO_OUTPUT_MKV && !conf->mux.disable_mkvext)
        muxer_to_be_used = MUXER_MKV;

    //muxerが必要ないかどうかチェック
    BOOL no_muxer = TRUE;
    no_muxer &= !audio_output;
    no_muxer &= !conf->vid.afs;
    no_muxer &= video_output_type == check_video_ouput(temp_filename);
    no_muxer &= !check_output_has_chapter(conf, sys_dat, muxer_to_be_used);
    return (no_muxer) ? MUXER_DISABLED : muxer_to_be_used;
}

AUO_RESULT getLogFilePath(char *log_file_path, size_t nSize, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    AUO_RESULT ret = AUO_RESULT_SUCCESS;
    guiEx_settings stg(TRUE); //ログウィンドウの保存先設定は最新のものを使用する
    stg.load_log_win();
    switch (stg.s_log.auto_save_log_mode) {
        case AUTO_SAVE_LOG_CUSTOM:
            char log_file_dir[MAX_PATH_LEN];
            strcpy_s(log_file_path, nSize, stg.s_log.auto_save_log_path);
            cmd_replace(log_file_path, nSize, pe, sys_dat, conf, oip);
            PathGetDirectory(log_file_dir, _countof(log_file_dir), log_file_path);
            if (DirectoryExistsOrCreate(log_file_dir))
                break;
            ret = AUO_RESULT_WARNING;
            //下へフォールスルー
        case AUTO_SAVE_LOG_OUTPUT_DIR:
        default:
            apply_appendix(log_file_path, nSize, oip->savefile, "_log.txt");
            break;
    }
    return ret;
}

//tc_filenameのタイムコードを分析して動画の長さを得て、
//duration(秒)にセットする
//fpsにはAviutlからの値を与える(参考として使う)
static AUO_RESULT get_duration_from_timecode(double *duration, const char *tc_filename, double fps) {
    AUO_RESULT ret = AUO_RESULT_SUCCESS;
    FILE *fp = NULL;
    *duration = 0.0;
    if (!(NULL == fopen_s(&fp, tc_filename, "r") && fp)) {
        //ファイルオープンエラー
        ret |= AUO_RESULT_ERROR;
    } else {
        const int avg_frames = 5; //平均をとるフレーム数
        char buf[256];
        double timecode[avg_frames];
        //ファイルからタイムコードを読み出し
        int frame = 0;
        while (fgets(buf, sizeof(buf), fp) != NULL) {
            if (buf[0] == '#')
                continue;
            if (1 != sscanf_s(buf, "%lf", &timecode[frame%avg_frames])) {
                ret |= AUO_RESULT_ERROR; break;
            }
            frame++;
        }
        fclose(fp);
        frame--; //最後のフレームに合わせる
        switch (frame) {
            case -1: //1フレーム分も読めなかった
                ret |= AUO_RESULT_ERROR; break;
            case 0: //1フレームのみ
                *duration = 1.0 / fps; break;
            default: //フレーム時間を求める((avg_frames-1)フレーム分から平均をとる)
                int div = 0, n = std::min(frame, avg_frames);
                double sum = 0.0;
                for (int i = 0; i < n; i++) {
                    sum += timecode[i];
                    div += i;
                }
                double frame_time = -1.0 * (sum - timecode[frame%avg_frames] * n) / (double)div;
                *duration = (timecode[frame%avg_frames] + frame_time) / 1000.0;
                break;
        }
    }
    return ret;
}

double get_duration(const CONF_GUIEX *conf, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe, const OUTPUT_INFO *oip) {
    char buffer[MAX_PATH_LEN];
    //Aviutlから再生時間情報を取得
    double duration = (((double)(oip->n + pe->delay_cut_additional_vframe) * (double)oip->scale) / (double)oip->rate);
#if ENABLE_TCFILE_IN
    //tcfile-inなら、動画の長さはタイムコードから取得する
    if (conf->enc.use_tcfilein || 0 == get_option_value(conf->vid.cmdex, "--tcfile-in", buffer, sizeof(buffer))) {
        double duration_tmp = 0.0;
        if (conf->enc.use_tcfilein)
            strcpy_s(buffer, sizeof(buffer), conf->vid.tcfile_in);
        cmd_replace(buffer, sizeof(buffer), pe, sys_dat, conf, oip);
        if (AUO_RESULT_SUCCESS == get_duration_from_timecode(&duration_tmp, buffer, oip->rate / (double)oip->scale))
            duration = duration_tmp;
        else
            warning_failed_to_get_duration_from_timecode();
    }
#endif
    return duration;
}

#if ENABLE_AMP

double get_amp_margin_bitrate(double base_bitrate, double margin_multi) {
    double clamp_offset = (margin_multi < 0.0) ? 0.2 : 0.0;
    return base_bitrate * clamp(1.0 - margin_multi / std::sqrt(std::max(base_bitrate, 1.0) / 100.0), 0.8 + clamp_offset, 1.0 + clamp_offset);
}

static AUO_RESULT amp_move_old_file(const char *muxout, const char *savefile) {
    if (!PathFileExists(muxout))
        return AUO_RESULT_ERROR;
    char filename[MAX_PATH_LEN];
    char appendix[MAX_APPENDIX_LEN];
    for (int i = 0; !i || PathFileExists(filename); i++) {
        sprintf_s(appendix, _countof(appendix), "_try%d%s", i, PathFindExtension(savefile));
        apply_appendix(filename, _countof(filename), savefile, appendix);
    }
    return (rename(muxout, filename) == 0) ? AUO_RESULT_SUCCESS : AUO_RESULT_ERROR;
}

static double get_vid_ratio(double actual_vid_bitrate, double vid_lower_limit_bitrate) {
    double vid_rate = actual_vid_bitrate / vid_lower_limit_bitrate;
    if (vid_lower_limit_bitrate < 1600) {
        //下限ビットレートが低い場合は、割り引いて考える
        vid_rate = 1.0 - (1.0 - vid_rate) / std::sqrt(1600.0 / vid_lower_limit_bitrate);
    }
    return vid_rate;
}

static double get_audio_bitrate(const PRM_ENC *pe, const OUTPUT_INFO *oip, double duration) {
    UINT64 aud_filesize = 0;
    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        for (int i_aud = 0; i_aud < pe->aud_count; i_aud++) {
            char aud_file[MAX_PATH_LEN];
            apply_appendix(aud_file, _countof(aud_file), pe->temp_filename, pe->append.aud[i_aud]);
            if (!PathFileExists(aud_file)) {
                error_no_aud_file(aud_file);
                return AUO_RESULT_ERROR;
            }
            UINT64 filesize_tmp = 0;
            if (!GetFileSizeUInt64(aud_file, &filesize_tmp)) {
                warning_failed_get_aud_size(aud_file); warning_amp_failed();
                return AUO_RESULT_ERROR;
            }
            aud_filesize += filesize_tmp;
        }
    }
    return (aud_filesize * 8.0) / 1000.0 / duration;
}

static void amp_adjust_lower_bitrate_set_default(CONF_X264 *cnf) {
    CONF_X264 x264_default = { 0 };
    get_default_conf_x264(&x264_default, cnf->use_highbit_depth);
    //すべてをデフォルトに戻すとcolormatrixなどのパラメータも戻ってしまうので、
    //エンコード速度に関係していそうなパラメータのみをデフォルトに戻す
    cnf->me = (std::min)(cnf->me, x264_default.me);
    cnf->me_range = (std::min)(cnf->me_range, x264_default.me_range);
    cnf->subme = (std::min)(cnf->subme, x264_default.subme);
    cnf->ref_frames = (std::min)(cnf->ref_frames, x264_default.ref_frames);
    cnf->trellis = (std::min)(cnf->trellis, x264_default.trellis);
    cnf->mb_partition &= x264_default.mb_partition;
    cnf->no_dct_decimate = x264_default.no_dct_decimate;
    cnf->no_fast_pskip = x264_default.no_fast_pskip;
}

static void amp_adjust_lower_bitrate_keyint(CONF_X264 *cnf, int keyint_div, int min_keyint) {
#define CEIL5(x) ((x >= 30) ? ((((x) + 4) / 5) * 5) : (x))
    min_keyint = (std::max)((std::min)(min_keyint, cnf->keyint_max / 2), 1);
    cnf->keyint_max = (std::max)((min_keyint), CEIL5(cnf->keyint_max / keyint_div));
#undef CEIL5
}

static void amp_adjust_lower_bitrate(CONF_X264 *cnf, int preset_idx, int preset_offset, int keyint_div, int min_keyint, const SYSTEM_DATA *sys_dat) {
    const int old_keyint = cnf->keyint_max;
    const int preset_new = (std::max)((std::min)((preset_idx), cnf->preset + (preset_offset)), 0);
    if (cnf->preset > preset_new) {
        amp_adjust_lower_bitrate_keyint(cnf, keyint_div, min_keyint);
        if (old_keyint != cnf->keyint_max) {
            cnf->preset = preset_new;
            write_log_auo_line_fmt(LOG_WARNING, g_auo_mes.get(AUO_ENCODE_AMP_ADJUST_LOW_BITRATE_PRESET_KEY),
                sys_dat->exstg->s_enc.preset.name[preset_new].name, cnf->keyint_max);
        } else {
            const int preset_adjust_new = (std::max)(preset_idx, 0);
            if (cnf->preset > preset_adjust_new) {
                cnf->preset = preset_adjust_new;
                write_log_auo_line_fmt(LOG_WARNING, g_auo_mes.get(AUO_ENCODE_AMP_ADJUST_LOW_BITRATE_PRESET),
                    sys_dat->exstg->s_enc.preset.name[cnf->preset].name);
            }
        }
    } else {
        amp_adjust_lower_bitrate_keyint(cnf, keyint_div, min_keyint);
        if (old_keyint != cnf->keyint_max) {
            write_log_auo_line_fmt(LOG_WARNING, g_auo_mes.get(AUO_ENCODE_AMP_ADJUST_LOW_BITRATE_KEY), cnf->keyint_max);
        }
    }
}

static AUO_RESULT amp_adjust_lower_bitrate_from_crf(CONF_X264 *cnf, const CONF_VIDEO *conf_vid, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe, const OUTPUT_INFO *oip, double duration, double file_bitrate) {
    //もし、もう設定を下げる余地がなければエラーを返す
    if (cnf->keyint_max == 1 && cnf->preset == 0) {
        return AUO_RESULT_ERROR;
    }
    const double aud_bitrate = get_audio_bitrate(pe, oip, duration);
    const double vid_bitrate = file_bitrate - aud_bitrate;
    //ビットレート倍率 = 今回のビットレート / 下限ビットレート
    const double vid_ratio = get_vid_ratio(vid_bitrate, (std::max)(1.0, conf_vid->amp_limit_bitrate_lower - aud_bitrate));
    //QPをいっぱいまで下げた時、このままの設定で下限ビットレートをクリアできそうなビットレート倍率
    //実際には動画によってcrfとビットレートの関係は異なるので、2次関数だと思って適当に近似計算
    const double est_max_vid_ratio = (std::min)(0.99, pow2(51.0 - cnf->crf * 0.01) / pow2(51.0));
    //QPを最大限引き下げられるように
    cnf->qp_min = 0;
    //デフォルトパラメータの一部を反映し、設定を軽くする
    if (vid_ratio < est_max_vid_ratio) {
        amp_adjust_lower_bitrate_set_default(cnf);
    }
    //キーフレーム間隔自動を反映
    if (cnf->keyint_max <= 0) {
        cnf->keyint_max = -1; //set_guiEx_auto_keyint()は -1 としておかないと自動設定を行わない
        set_guiEx_auto_keyint(cnf, oip->rate, oip->scale);
    }
#define ADJUST(preset_idx, preset_offset, keyint_div, min_keyint) amp_adjust_lower_bitrate(cnf, (preset_idx), (preset_offset), (keyint_div), (min_keyint), sys_dat)
    //HD解像度の静止画動画では、キーフレームの比重が大きいため、キーフレーム追加はやや控えめに
    bool bHD = oip->w * oip->h >= 1280 * 720;
    //「いい感じ」(試行錯誤の結果)(つまり適当) にプリセットとキーフレーム間隔を調整する
    if (       vid_ratio < est_max_vid_ratio * 0.05) {
        ADJUST(0, -3, 100, 2);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.08 : 0.10)) {
        ADJUST(0, -3, 60, 3);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.12 : 0.15)) {
        ADJUST(0, -3, 30, 5);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.15 : 0.20)) {
        ADJUST(0, -3, 25, 10);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.20 : 0.30)) {
        ADJUST(0, -3, 20, 10);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.30 : 0.50)) {
        ADJUST(0, -3, 15, 10);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.50 : 0.60)) {
        ADJUST(1, -3, 15, 15);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.60 : 0.70)) {
        ADJUST(1, -3, 12, 15);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.67 : 0.75)) {
        ADJUST(1, -3, 10, 15);
    } else if (vid_ratio < est_max_vid_ratio * ((bHD) ? 0.75 : 0.80)) {
        ADJUST(1, -3, 5, 15);
    } else if (vid_ratio < est_max_vid_ratio * 0.90) {
        ADJUST(1, -2, 5, 15);
    } else if (vid_ratio < est_max_vid_ratio * 0.95) {
        ADJUST(1, -2, 4, 15);
    } else if (vid_ratio < est_max_vid_ratio) {
        ADJUST(2, -1, 4, 15);
    } else {
        ADJUST(3, -1, 4, 30);
    }
#undef ADJUST
    apply_presets(cnf);
    cnf->qp_min = (std::min)(cnf->qp_min, 0);
    return AUO_RESULT_SUCCESS;
}

static AUO_RESULT amp_adjust_lower_bitrate_from_bitrate(CONF_X264 *cnf, const CONF_VIDEO *conf_vid, const SYSTEM_DATA *sys_dat, PRM_ENC *pe, const OUTPUT_INFO *oip, double duration, double file_bitrate) {
    const double aud_bitrate = get_audio_bitrate(pe, oip, duration);
    const double vid_bitrate = file_bitrate - aud_bitrate;
    //ビットレート倍率 = 今回のビットレート / 下限ビットレート
    const double vid_ratio = get_vid_ratio(vid_bitrate, (std::max)(1.0, conf_vid->amp_limit_bitrate_lower - aud_bitrate));
    if (vid_ratio < 0.98) {
        amp_adjust_lower_bitrate_set_default(cnf);
    }
    AUO_RESULT ret = AUO_RESULT_SUCCESS;
    //キーフレーム数を増やして1pass目からやり直す
    pe->amp_reset_pass_count++;
    pe->total_pass--;
    pe->current_pass = 1;
    cnf->qp_min = (std::min)(cnf->qp_min, 0);
    cnf->slow_first_pass = FALSE;
    cnf->nul_out = TRUE;
    //ここでは目標ビットレートを0に指定しておき、後段のcheck_ampで上限/下限設定をもとに修正させる
    cnf->bitrate = 0;
    cnf->crf = 2;
    ret = amp_adjust_lower_bitrate_from_crf(cnf, conf_vid, sys_dat, pe, oip, duration, file_bitrate);
    if (ret == AUO_RESULT_SUCCESS) {
        ret = AUO_RESULT_WARNING;
    }
    return ret;
}

//戻り値
//  0 … チェック完了
// -1 … チェックできない(エラー)
//  1 … 動画を再エンコ
//  2 … 音声を再エンコ
int amp_check_file(CONF_GUIEX *conf, const SYSTEM_DATA *sys_dat, PRM_ENC *pe, const OUTPUT_INFO *oip) {
    if (!conf->enc.use_auto_npass || !conf->vid.amp_check || conf->oth.out_audio_only)
        return 0;
    //チェックするファイル名を取得
    char muxout[MAX_PATH_LEN];
    if (PathFileExists(pe->temp_filename)) {
        strcpy_s(muxout, _countof(muxout), pe->temp_filename);
    } else {
        //tempfileがない場合、mux後ファイルをチェックする
        get_muxout_filename(muxout, _countof(muxout), sys_dat, pe);
        if (pe->muxer_to_be_used < 0 || !PathFileExists(muxout)) {
            error_check_muxout_exist(muxout); warning_amp_failed();
            return -1;
        }
    }
    //ファイルサイズを取得し、ビットレートを計算する
    UINT64 filesize = 0;
    if (!GetFileSizeUInt64(muxout, &filesize)) {
        warning_failed_check_muxout_filesize(muxout); warning_amp_failed();
        return -1;
    }
    const double duration = get_duration(conf, sys_dat, pe, oip);
    double file_bitrate = (filesize * 8.0) / 1000.0 / duration;
    DWORD status = 0x00;
    //ファイルサイズのチェックを行う
    if ((conf->vid.amp_check & AMPLIMIT_FILE_SIZE) && filesize > conf->vid.amp_limit_file_size * 1024*1024)
        status |= AMPLIMIT_FILE_SIZE;
    //ビットレートのチェックを行う
    if ((conf->vid.amp_check & AMPLIMIT_BITRATE_UPPER) && file_bitrate > conf->vid.amp_limit_bitrate_upper)
        status |= AMPLIMIT_BITRATE_UPPER;
    if ((conf->vid.amp_check & AMPLIMIT_BITRATE_LOWER) && file_bitrate < conf->vid.amp_limit_bitrate_lower)
        status |= AMPLIMIT_BITRATE_LOWER;

    BOOL retry = (status && pe->current_pass < pe->amp_pass_limit && pe->amp_reset_pass_count < pe->amp_reset_pass_limit);
    BOOL show_header = FALSE;
    int amp_result = 0;
    bool amp_crf_reenc = false;
    //再エンコードを行う
    if (retry) {
        //muxerを再設定する
        pe->muxer_to_be_used = check_muxer_to_be_used(conf, sys_dat, pe->temp_filename, pe->video_out_type, (oip->flag & OUTPUT_INFO_FLAG_AUDIO) != 0);

        //まずビットレートの上限を計算
        double limit_bitrate_upper = DBL_MAX;
        if (status & AMPLIMIT_FILE_SIZE)
            limit_bitrate_upper = std::min(limit_bitrate_upper, (conf->vid.amp_limit_file_size * 1024*1024)*8.0/1000 / duration);
        if (status & AMPLIMIT_BITRATE_UPPER)
            limit_bitrate_upper = std::min(limit_bitrate_upper, conf->vid.amp_limit_bitrate_upper);
        //次にビットレートの下限を計算
        double limit_bitrate_lower = (status & AMPLIMIT_BITRATE_LOWER) ? conf->vid.amp_limit_bitrate_lower : 0.0;
        //上限・下限チェック
        if (limit_bitrate_lower > limit_bitrate_upper) {
            warning_amp_bitrate_confliction((int)limit_bitrate_lower, (int)limit_bitrate_upper);
            conf->vid.amp_check &= ~AMPLIMIT_BITRATE_LOWER;
            limit_bitrate_lower = 0.0;
        }
        //必要な修正量を算出
        //deltaは上げる必要があれば正、下げる必要があれば負
        double bitrate_delta = 0.0;
        if (file_bitrate > limit_bitrate_upper) {
            bitrate_delta = limit_bitrate_upper - file_bitrate;
        }
        if (file_bitrate < limit_bitrate_lower) {
            bitrate_delta = limit_bitrate_lower - file_bitrate;
        }
        //音声がビットレートモードなら音声再エンコによる調整を検討する
        const AUDIO_SETTINGS *aud_stg = &sys_dat->exstg->s_aud[conf->aud.encoder];
        if ((oip->flag & OUTPUT_INFO_FLAG_AUDIO)
            && bitrate_delta < 0.0 //ビットレートを上げるのに音声再エンコするのはうまくいかないことが多い
            && aud_stg->mode[conf->aud.enc_mode].bitrate
            && 16.0 < conf->aud.bitrate * sys_dat->exstg->s_local.amp_reenc_audio_multi //最低でも16kbpsは動かしたほうが良い
            && std::abs(bitrate_delta) + 1.0 < conf->aud.bitrate * sys_dat->exstg->s_local.amp_reenc_audio_multi //ビットレート変化は閾値の範囲内
            && str_has_char(pe->muxed_vid_filename)
            && PathFileExists(pe->muxed_vid_filename)) {
            //音声の再エンコードで修正
            amp_result = 2;
            const int delta_sign = (bitrate_delta >= 0.0) ? 1 : -1;
            conf->aud.bitrate += (int)((std::max)(std::abs(bitrate_delta), (std::min)(15.0, conf->aud.bitrate * (1.0 / 8.0))) + 1.5) * delta_sign;

            //動画のみファイルをもとの位置へ
            remove(pe->temp_filename);
            char temp_ext[MAX_APPENDIX_LEN];
            strcpy_s(temp_ext, _countof(temp_ext), VID_FILE_APPENDIX);
            strcat_s(temp_ext, _countof(temp_ext), PathFindExtension(pe->temp_filename));
            replace(pe->temp_filename, _countof(pe->temp_filename), temp_ext, temp_ext + strlen(VID_FILE_APPENDIX));
            if (PathFileExists(pe->temp_filename)) remove(pe->temp_filename);
            rename(pe->muxed_vid_filename, pe->temp_filename);

            //音声エンコードではヘッダーが表示されないので、 ここで表示しておく
            show_header = TRUE;
        } else {
            //動画の再エンコードで修正
            amp_result = 1;
            pe->total_pass++;
            if (conf->enc.rc_mode == X264_RC_CRF) {
                //上限確認付 品質基準VBR(可変レート)の場合、自動的に再設定
                pe->amp_pass_limit++;
                pe->current_pass = 1;
                conf->enc.rc_mode = X264_RC_BITRATE;
                conf->enc.slow_first_pass = FALSE;
                conf->enc.nul_out = TRUE;
                //ここでは目標ビットレートを上限を上回った場合には-1、下限を下回った場合には0に指定しておき、
                //後段のcheck_ampで上限/下限設定をもとに修正させる
                conf->enc.bitrate = (bitrate_delta < 0) ? -1 : 0;
                //自動マルチパスの1pass目には本来ヘッダーが表示されないので、 ここで表示しておく
                show_header = TRUE;
                amp_crf_reenc = true;
                //下限を下回った場合
                if (bitrate_delta > 0) {
                    //下限を大きく下回っていたら、単に2passエンコするだけでは不十分
                    pe->amp_reset_pass_count++;
                    if (amp_adjust_lower_bitrate_from_crf(&conf->enc, &conf->vid, sys_dat, pe, oip, duration, file_bitrate) != AUO_RESULT_SUCCESS) {
                        retry = FALSE;
                        amp_result = 0;
                    }
                }
            } else {
                //再エンコ時は現在の目標ビットレートより少し下げたレートでエンコーダを行う
                //新しい目標ビットレートを4通りの方法で計算してみる
                double margin_bitrate = get_amp_margin_bitrate(conf->enc.bitrate, sys_dat->exstg->s_local.amp_bitrate_margin_multi * ((status & (AMPLIMIT_FILE_SIZE | AMPLIMIT_BITRATE_UPPER)) ? 0.5 : -4.0));
                double bitrate_limit_upper = (conf->vid.amp_check & AMPLIMIT_BITRATE_UPPER) ? conf->enc.bitrate - 0.5 * (file_bitrate - conf->vid.amp_limit_bitrate_upper) : DBL_MAX;
                double bitrate_limit_lower = (conf->vid.amp_check & AMPLIMIT_BITRATE_LOWER) ? conf->enc.bitrate + 0.5 * (conf->vid.amp_limit_bitrate_lower - file_bitrate) : 0.0;
                double filesize_limit = (conf->vid.amp_check & AMPLIMIT_FILE_SIZE) ? conf->enc.bitrate - 0.5 * ((filesize - conf->vid.amp_limit_file_size*1024*1024))* 8.0/1000.0 / get_duration(conf, sys_dat, pe, oip) : conf->enc.bitrate;
                conf->enc.bitrate = (int)(0.5 + std::max(std::min(margin_bitrate, std::min(filesize_limit, bitrate_limit_upper)), bitrate_limit_lower));
                if (conf->vid.amp_check & AMPLIMIT_BITRATE_LOWER) {
                    AUO_RESULT ret = amp_adjust_lower_bitrate_from_bitrate(&conf->enc, &conf->vid, sys_dat, pe, oip, duration, file_bitrate);
                    if (ret == AUO_RESULT_WARNING) {
                        //1pass目からやり直し
                        show_header = TRUE;
                        amp_crf_reenc = true;
                    } else if (ret != AUO_RESULT_SUCCESS) {
                        retry = FALSE;
                        amp_result = 0;
                    }
                }
            }
            //必要なら、今回作成した動画を待避
            if (sys_dat->exstg->s_local.amp_keep_old_file)
                amp_move_old_file(muxout, oip->savefile);
        }
    }
    info_amp_result(status, amp_result, filesize, file_bitrate, conf->vid.amp_limit_file_size, conf->vid.amp_limit_bitrate_upper, conf->vid.amp_limit_bitrate_lower, (std::max)(pe->amp_reset_pass_count, pe->current_pass - conf->enc.auto_npass), (amp_result == 2) ? conf->aud.bitrate : conf->enc.bitrate);

    if (show_header)
        open_log_window(oip, sys_dat, pe->current_pass, pe->total_pass, amp_crf_reenc);

    return amp_result;
}

#endif

int ReadLogExe(PIPE_SET *pipes, const wchar_t *exename, LOG_CACHE *log_line_cache) {
    DWORD pipe_read = 0;
    if (pipes->stdOut.h_read) {
        if (!PeekNamedPipe(pipes->stdOut.h_read, NULL, 0, NULL, &pipe_read, NULL))
            return -1;
        if (pipe_read) {
            ReadFile(pipes->stdOut.h_read, pipes->read_buf + pipes->buf_len, sizeof(pipes->read_buf) - pipes->buf_len - 1, &pipe_read, NULL);
            pipes->buf_len += pipe_read;
            pipes->read_buf[pipes->buf_len] = '\0';
            write_log_exe_mes(pipes->read_buf, &pipes->buf_len, exename, log_line_cache);
        }
    }
    return (int)pipe_read;
}

void write_cached_lines(int log_level, const wchar_t *exename, LOG_CACHE *log_line_cache) {
    static const wchar_t *const LOG_LEVEL_STR[] = { L"info", L"warning", L"error" };
    static const wchar_t *MESSAGE_FORMAT = L"%s [%s]: %s";
    wchar_t *buffer = NULL;
    int buffer_len = 0;
    const int log_level_idx = clamp(log_level, LOG_INFO, LOG_ERROR);
    const int additional_length = wcslen(exename) + wcslen(LOG_LEVEL_STR[log_level_idx]) + wcslen(MESSAGE_FORMAT) - wcslen(L"%s") * 3 + 1;
    for (int i = 0; i < log_line_cache->idx; i++) {
        const int required_buffer_len = wcslen(log_line_cache->lines[i]) + additional_length;
        if (buffer_len < required_buffer_len) {
            if (buffer) free(buffer);
            buffer = (wchar_t *)malloc(required_buffer_len * sizeof(buffer[0]));
            buffer_len = required_buffer_len;
        }
        if (buffer) {
            swprintf_s(buffer, buffer_len, MESSAGE_FORMAT, exename, LOG_LEVEL_STR[log_level_idx], log_line_cache->lines[i]);
            write_log_line(log_level, buffer);
        }
    }
    if (buffer) free(buffer);
}

#include <tlhelp32.h>

static bool check_parent(size_t check_pid, const size_t target_pid, const std::unordered_map<size_t, size_t>& map_pid) {
    for (size_t i = 0; i < map_pid.size(); i++) { // 最大でもmap_pid.size()を超えてチェックする必要はないはず
        if (check_pid == target_pid) return true;
        if (check_pid == 0) return false;
        auto key = map_pid.find(check_pid);
        if (key == map_pid.end() || key->second == 0 || key->second == key->first) return false;
        check_pid = key->second;
    }
    return false;
};

static std::vector<size_t> createChildProcessIDList(const size_t target_pid) {
    auto h = unique_handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
    if (h.get() == INVALID_HANDLE_VALUE) {
        return std::vector<size_t>();
    }

    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    std::unordered_map<size_t, size_t> map_pid;
    if (Process32First(h.get(), &pe)) {
        do {
            map_pid[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32Next(h.get(), &pe));
    }

    std::vector<size_t> list_childs;
    for (auto& [pid, parentpid] : map_pid) {
        if (check_parent(parentpid, target_pid, map_pid)) {
            list_childs.push_back(pid);
        }
    }
    return list_childs;
}

// ----------------------------------------------------------------------------------------------------------------

#include <winternl.h>

typedef __kernel_entry NTSYSCALLAPI NTSTATUS(NTAPI *NtQueryObject_t)(HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength);
typedef __kernel_entry NTSTATUS(NTAPI *NtQuerySystemInformation_t)(SYSTEM_INFORMATION_CLASS SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR  NumberOfHandles;
    ULONG_PTR  Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, * PSYSTEM_HANDLE_INFORMATION_EX;

#pragma warning(push)
#pragma warning(disable: 4200) //C4200: 非標準の拡張機能が使用されています: 構造体または共用体中にサイズが 0 の配列があります。
typedef struct _OBJECT_NAME_INFORMATION {
    UNICODE_STRING          Name;
    WCHAR                   NameBuffer[0];
} OBJECT_NAME_INFORMATION, * POBJECT_NAME_INFORMATION;
#pragma warning(pop)

typedef struct _OBJECT_BASIC_INFORMATION {
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG PagedPoolCharge;
    ULONG NonPagedPoolCharge;
    ULONG Reserved[3];
    ULONG NameInfoSize;
    ULONG TypeInfoSize;
    ULONG SecurityDescriptorSize;
    LARGE_INTEGER CreationTime;
} OBJECT_BASIC_INFORMATION, *POBJECT_BASIC_INFORMATION;

typedef struct _OBJECT_TYPE_INFORMATION {
    UNICODE_STRING TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    UCHAR TypeIndex; // since WINBLUE
    CHAR ReservedByte;
    ULONG PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
} OBJECT_TYPE_INFORMATION, *POBJECT_TYPE_INFORMATION;

typedef struct _OBJECT_TYPES_INFORMATION {
    ULONG NumberOfTypes;
} OBJECT_TYPES_INFORMATION, *POBJECT_TYPES_INFORMATION;

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

#define CEIL_INT(x, div) (((x + div - 1) / div) * div)

static std::vector<unique_handle> createProcessHandleList(const std::vector<size_t>& list_pid, const wchar_t *handle_type) {
    std::vector<unique_handle> handle_list;
    std::unique_ptr<std::remove_pointer<HMODULE>::type, decltype(&FreeLibrary)> hNtDll(LoadLibrary(_T("ntdll.dll")), FreeLibrary);
    if (hNtDll == NULL) return handle_list;

    auto fNtQueryObject = (decltype(NtQueryObject) *)GetProcAddress(hNtDll.get(), "NtQueryObject");
    auto fNtQuerySystemInformation = (decltype(NtQuerySystemInformation) *)GetProcAddress(hNtDll.get(), "NtQuerySystemInformation");
    if (fNtQueryObject == nullptr || fNtQuerySystemInformation == nullptr) {
        return handle_list;
    }

    //auto getObjectTypeNumber = [fNtQueryObject](wchar_t * TypeName) {
    //    static const auto ObjectTypesInformation = (OBJECT_INFORMATION_CLASS)3;
    //    std::vector<char> data(1024, 0);
    //    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    //    do {
    //        data.resize(data.size() * 2);
    //        ULONG size = 0;
    //        status = fNtQueryObject(NULL, ObjectTypesInformation, data.data(), data.size(), &size);
    //    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    //    POBJECT_TYPES_INFORMATION objectTypes = (POBJECT_TYPES_INFORMATION)data.data();
    //    char *ptr = data.data() + CEIL_INT(sizeof(OBJECT_TYPES_INFORMATION), sizeof(ULONG_PTR));
    //    for (size_t i = 0; i < objectTypes->NumberOfTypes; i++) {
    //        POBJECT_TYPE_INFORMATION objectType = (POBJECT_TYPE_INFORMATION)ptr;
    //        if (wcsicmp(objectType->TypeName.Buffer, TypeName) == 0) {
    //            return (int)objectType->TypeIndex;
    //        }
    //        ptr += sizeof(OBJECT_TYPE_INFORMATION) + CEIL_INT(objectType->TypeName.MaximumLength, sizeof(ULONG_PTR));
    //    }
    //    return -1;
    //};
    //const int fileObjectTypeIndex = getObjectTypeNumber(L"File");

    static const SYSTEM_INFORMATION_CLASS SystemExtendedHandleInformation = (SYSTEM_INFORMATION_CLASS)0x40;
    ULONG size = 0;
    fNtQuerySystemInformation(SystemExtendedHandleInformation, NULL, 0, &size);
    std::vector<char> shibuffer;
    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    do {
        shibuffer.resize(size + 16*1024);
        status = fNtQuerySystemInformation(SystemExtendedHandleInformation, shibuffer.data(), shibuffer.size(), &size);
    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    if (NT_SUCCESS(status)) {
        const auto currentPID = GetCurrentProcessId();
        const auto currentProcessHandle = GetCurrentProcess();
        const auto shi = (PSYSTEM_HANDLE_INFORMATION_EX)shibuffer.data();
        for (decltype(shi->NumberOfHandles) i = 0; i < shi->NumberOfHandles; i++) {
            const auto handlePID = shi->Handles[i].UniqueProcessId;
            if (std::find(list_pid.begin(), list_pid.end(), handlePID) != list_pid.end()) {
                auto handle = unique_handle((HANDLE)shi->Handles[i].HandleValue, []([[maybe_unused]] HANDLE h) { /*Do nothing*/ });
                // handleValue はプロセスごとに存在する
                // 自プロセスでなければ、DuplicateHandle で自プロセスでの調査用のhandleをつくる
                // その場合は新たに作ったhandleなので CloseHandle が必要
                if (shi->Handles[i].UniqueProcessId != currentPID) {
                    const auto hProcess = std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(&CloseHandle)>(OpenProcess(PROCESS_DUP_HANDLE, FALSE, handlePID), CloseHandle);
                    if (hProcess) {
                        HANDLE handleDup = NULL;
                        const BOOL ret = DuplicateHandle(hProcess.get(), (HANDLE)shi->Handles[i].HandleValue, currentProcessHandle, &handleDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
                        if (ret) {
                            handle = unique_handle((HANDLE)handleDup, [](HANDLE h) { CloseHandle(h); });
                        }
                    }
                }
                if (handle_type) {
                    // handleの種類を確認する
                    size = 0;
                    status = fNtQueryObject(handle.get(), ObjectTypeInformation, NULL, 0, &size);
                    if (status == STATUS_INFO_LENGTH_MISMATCH) { // 問題なければ、STATUS_INFO_LENGTH_MISMATCHが返る
                        std::vector<char> otibuffer(size, 0);
                        status = fNtQueryObject(handle.get(), ObjectTypeInformation, otibuffer.data(), otibuffer.size(), &size);
                        const auto oti = (PPUBLIC_OBJECT_TYPE_INFORMATION)otibuffer.data();
                        if (NT_SUCCESS(status) && oti->TypeName.Buffer && _wcsicmp(oti->TypeName.Buffer, handle_type) == 0) {
                            //static const OBJECT_INFORMATION_CLASS ObjectNameInformation = (OBJECT_INFORMATION_CLASS)1;
                            //status = fNtQueryObject(handle, ObjectNameInformation, NULL, 0, &size);
                            //std::vector<char> buffer3(size, 0);
                            //status = fNtQueryObject(handle, ObjectNameInformation, buffer3.data(), buffer3.size(), &size);
                            //POBJECT_NAME_INFORMATION oni = (POBJECT_NAME_INFORMATION)buffer3.data();
                            handle_list.push_back(std::move(handle));
                        }
                    }
                } else {
                    handle_list.push_back(std::move(handle));
                }
            }
        }
    }
    return handle_list;
}

static std::vector<std::basic_string<TCHAR>> createProcessOpenedFileList(const std::vector<size_t>& list_pid) {
    const auto list_handle = createProcessHandleList(list_pid, L"File");
    std::vector<std::basic_string<TCHAR>> list_file;
    std::vector<TCHAR> filename(32768+1, 0);
    for (const auto& handle : list_handle) {
        const auto fileType = GetFileType(handle.get());
        if (fileType == FILE_TYPE_DISK) { //ハンドルがパイプだとGetFinalPathNameByHandleがフリーズするため使用不可
            memset(filename.data(), 0, sizeof(filename[0]) * filename.size());
            auto ret = GetFinalPathNameByHandle(handle.get(), filename.data(), filename.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (ret != 0) {
                try {
                    auto f = std::filesystem::canonical(filename.data());
                    if (std::filesystem::is_regular_file(f)) {
                        list_file.push_back(f.string<TCHAR>());
                    }
                } catch (...) {}
            }
        }
    }
    // 重複を排除
    std::sort(list_file.begin(), list_file.end());
    auto result = std::unique(list_file.begin(), list_file.end());
    // 不要になった要素を削除
    list_file.erase(result, list_file.end());
    return list_file;
}

static std::vector<std::wstring> createProcessModuleList() {
    std::vector<std::wstring> moduleList;
    const auto currentPID = GetCurrentProcessId();
    std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(&CloseHandle)> hProcess(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, currentPID), CloseHandle);
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    if (EnumProcessModules(hProcess.get(), hMods, sizeof(hMods), &cbNeeded)) {
        for (size_t i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            wchar_t moduleName[MAX_PATH_LEN] = { 0 };
            if (GetModuleFileNameExW(hProcess.get(), hMods[i], moduleName, _countof(moduleName))) {
                moduleList.push_back(moduleName);
            }
        }
    }
    return moduleList;
}

bool checkIfModuleLoaded(const wchar_t *moduleName) {
    const auto moduleList = createProcessModuleList();
    for (const auto& modulePath : moduleList) {
        const auto moduleFilename = std::filesystem::path(modulePath).filename().wstring();
        if (_wcsicmp(moduleName, moduleFilename.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

static bool rgy_path_is_same(const TCHAR *path1, const TCHAR *path2) {
    try {
        const auto p1 = std::filesystem::path(path1);
        const auto p2 = std::filesystem::path(path2);
        std::error_code ec;
        return std::filesystem::equivalent(p1, p2, ec);
    } catch (...) {
        return false;
    }
}

static void create_aviutl_opened_file_list(PRM_ENC *pe) {
    const auto pid_aviutl = GetCurrentProcessId();
    auto list_pid = createChildProcessIDList(pid_aviutl);
    list_pid.push_back(pid_aviutl);

    const auto list_file = createProcessOpenedFileList(list_pid);
    pe->n_opened_aviutl_files = (int)list_file.size();
    if (pe->n_opened_aviutl_files > 0) {
        pe->opened_aviutl_files = (char **)calloc(1, sizeof(char *) * pe->n_opened_aviutl_files);
        for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
            pe->opened_aviutl_files[i] = _strdup(list_file[i].c_str());
        }
    }
}

static bool check_file_is_aviutl_opened_file(const char *filepath, const PRM_ENC *pe) {
    for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
        if (rgy_path_is_same(filepath, pe->opened_aviutl_files[i])) {
            return true;
        }
    }
    return false;
}
