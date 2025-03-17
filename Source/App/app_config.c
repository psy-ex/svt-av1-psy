/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 3-Clause Clear License and
* the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>

#include "EbSvtAv1Metadata.h"
#include "app_config.h"
#include "app_context.h"
#include "app_input_y4m.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/file.h>
#endif

#include "app_output_ivf.h"

#if !defined(_WIN32) || !defined(HAVE_STRNLEN_S)
#include "third_party/safestringlib/safe_str_lib.h"
#endif

/**********************************
 * Defines
 **********************************/
#define HELP_TOKEN "--help"
#define COLORH_TOKEN "--color-help"
#define VERSION_TOKEN "--version"
#define CHANNEL_NUMBER_TOKEN "--nch"
#define COMMAND_LINE_MAX_SIZE 2048
#define CONFIG_FILE_TOKEN "-c"
#define CONFIG_FILE_LONG_TOKEN "--config"
#define INPUT_FILE_TOKEN "-i"
#define OUTPUT_BITSTREAM_TOKEN "-b"
#define OUTPUT_RECON_TOKEN "-o"
#define ERROR_FILE_TOKEN "--errlog"

/* two pass token */
#define PASS_TOKEN "--pass"
#define TWO_PASS_STATS_TOKEN "--stats"
#define PASSES_TOKEN "--passes"
#define STAT_FILE_TOKEN "--stat-file"
#define WIDTH_TOKEN "-w"
#define HEIGHT_TOKEN "-h"
#define NUMBER_OF_PICTURES_TOKEN "-n"
#define BUFFERED_INPUT_TOKEN "--nb"
#define NO_PROGRESS_TOKEN "--no-progress" // tbd if it should be removed
#define PROGRESS_TOKEN "--progress"
#define QP_TOKEN "-q"
#define USE_QP_FILE_TOKEN "--use-q-file"
#define FORCE_KEY_FRAMES_TOKEN "--force-key-frames"

#define USE_FIXED_QINDEX_OFFSETS_TOKEN "--use-fixed-qindex-offsets"
#define QINDEX_OFFSETS_TOKEN "--qindex-offsets"
#define KEY_FRAME_QINDEX_OFFSET_TOKEN "--key-frame-qindex-offset"
#define KEY_FRAME_CHROMA_QINDEX_OFFSET_TOKEN "--key-frame-chroma-qindex-offset"
#define CHROMA_QINDEX_OFFSETS_TOKEN "--chroma-qindex-offsets"
#define LUMA_Y_DC_QINDEX_OFFSET_TOKEN "--luma-y-dc-qindex-offset"
#define CHROMA_U_DC_QINDEX_OFFSET_TOKEN "--chroma-u-dc-qindex-offset"
#define CHROMA_U_AC_QINDEX_OFFSET_TOKEN "--chroma-u-ac-qindex-offset"
#define CHROMA_V_DC_QINDEX_OFFSET_TOKEN "--chroma-v-dc-qindex-offset"
#define CHROMA_V_AC_QINDEX_OFFSET_TOKEN "--chroma-v-ac-qindex-offset"

// scale factors for lambda value for different frame types
#define LAMBDA_SCALE_FACTORS_TOKEN "--lambda-scale-factors"

#define FRAME_RATE_TOKEN "--fps"
#define FRAME_RATE_NUMERATOR_TOKEN "--fps-num"
#define FRAME_RATE_DENOMINATOR_TOKEN "--fps-denom"
#define ENCODER_COLOR_FORMAT "--color-format"
#define HIERARCHICAL_LEVELS_TOKEN "--hierarchical-levels" // no Eval
#define PRED_STRUCT_TOKEN "--pred-struct"
#define PROFILE_TOKEN "--profile"
#define INTRA_PERIOD_TOKEN "--intra-period"
#define TIER_TOKEN "--tier"
#define LEVEL_TOKEN "--level"
#define FILM_GRAIN_TOKEN "--film-grain"
#define FILM_GRAIN_DENOISE_APPLY_TOKEN "--film-grain-denoise"
#define INTRA_REFRESH_TYPE_TOKEN "--irefresh-type" // no Eval
#define CDEF_ENABLE_TOKEN "--enable-cdef"
#define SCREEN_CONTENT_TOKEN "--scm"
// --- start: ALTREF_FILTERING_SUPPORT
#define ENABLE_TF_TOKEN "--enable-tf"
#define ENABLE_OVERLAYS "--enable-overlays"
#define TUNE_TOKEN "--tune"
// --- end: ALTREF_FILTERING_SUPPORT
// --- start: SUPER-RESOLUTION SUPPORT
#define SUPERRES_MODE_INPUT "--superres-mode"
#define SUPERRES_DENOM "--superres-denom"
#define SUPERRES_KF_DENOM "--superres-kf-denom"
#define SUPERRES_QTHRES "--superres-qthres"
#define SUPERRES_KF_QTHRES "--superres-kf-qthres"
// --- end: SUPER-RESOLUTION SUPPORT
// --- start: REFERENCE SCALING SUPPORT
#define RESIZE_MODE_INPUT "--resize-mode"
#define RESIZE_DENOM "--resize-denom"
#define RESIZE_KF_DENOM "--resize-kf-denom"
#define RESIZE_FRAME_EVTS "--frame-resz-events"
#define RESIZE_FRAME_KF_DENOMS "--frame-resz-kf-denoms"
#define RESIZE_FRAME_DENOMS "--frame-resz-denoms"
// --- end: REFERENCE SCALING SUPPORT
#define RATE_CONTROL_ENABLE_TOKEN "--rc"
#define TARGET_BIT_RATE_TOKEN "--tbr"
#define MAX_BIT_RATE_TOKEN "--mbr"
#define MAX_QP_TOKEN "--max-qp"
#define MIN_QP_TOKEN "--min-qp"
#define VBR_MIN_SECTION_PCT_TOKEN "--minsection-pct"
#define VBR_MAX_SECTION_PCT_TOKEN "--maxsection-pct"
#define UNDER_SHOOT_PCT_TOKEN "--undershoot-pct"
#define OVER_SHOOT_PCT_TOKEN "--overshoot-pct"
#define MBR_OVER_SHOOT_PCT_TOKEN "--mbr-overshoot-pct"
#define GOP_CONSTRAINT_RC_TOKEN "--gop-constraint-rc"
#define BUFFER_SIZE_TOKEN "--buf-sz"
#define BUFFER_INITIAL_SIZE_TOKEN "--buf-initial-sz"
#define BUFFER_OPTIMAL_SIZE_TOKEN "--buf-optimal-sz"
#define RECODE_LOOP_TOKEN "--recode-loop"
#define ENABLE_TPL_LA_TOKEN "--enable-tpl-la"
#define TILE_ROW_TOKEN "--tile-rows"
#define TILE_COL_TOKEN "--tile-columns"

#define SCENE_CHANGE_DETECTION_TOKEN "--scd"
#define INJECTOR_TOKEN "--inj" // no Eval
#define INJECTOR_FRAMERATE_TOKEN "--inj-frm-rt" // no Eval
#define ASM_TYPE_TOKEN "--asm"
#define THREAD_MGMNT "--lp"
#define PIN_TOKEN "--pin"
#define TARGET_SOCKET "--ss"

//double dash
#define PRESET_TOKEN "--preset"
#define QP_FILE_NEW_TOKEN "--qpfile"
#define INPUT_DEPTH_TOKEN "--input-depth"
#define KEYINT_TOKEN "--keyint"
#define LOOKAHEAD_NEW_TOKEN "--lookahead"
#define SVTAV1_PARAMS "--svtav1-params"

#define STAT_REPORT_NEW_TOKEN "--enable-stat-report"
#define ENABLE_RESTORATION_TOKEN "--enable-restoration"
#define MFMV_ENABLE_NEW_TOKEN "--enable-mfmv"
#define DG_ENABLE_NEW_TOKEN "--enable-dg"
#define FAST_DECODE_TOKEN "--fast-decode"
#define ADAPTIVE_QP_ENABLE_NEW_TOKEN "--aq-mode"
#define INPUT_FILE_LONG_TOKEN "--input"
#define OUTPUT_BITSTREAM_LONG_TOKEN "--output"
#define OUTPUT_RECON_LONG_TOKEN "--recon"
#define WIDTH_LONG_TOKEN "--width"
#define HEIGHT_LONG_TOKEN "--height"
#define NUMBER_OF_PICTURES_LONG_TOKEN "--frames"
#define NUMBER_OF_PICTURES_TO_SKIP "--skip"

#define QP_LONG_TOKEN "--qp"
#define CRF_LONG_TOKEN "--crf"
#define LOOP_FILTER_ENABLE "--enable-dlf"
#define FORCED_MAX_FRAME_WIDTH_TOKEN "--forced-max-frame-width"
#define FORCED_MAX_FRAME_HEIGHT_TOKEN "--forced-max-frame-height"

#define COLOR_PRIMARIES_NEW_TOKEN "--color-primaries"
#define TRANSFER_CHARACTERISTICS_NEW_TOKEN "--transfer-characteristics"
#define MATRIX_COEFFICIENTS_NEW_TOKEN "--matrix-coefficients"
#define COLOR_RANGE_NEW_TOKEN "--color-range"
#define CHROMA_SAMPLE_POSITION_TOKEN "--chroma-sample-position"
#define MASTERING_DISPLAY_TOKEN "--mastering-display"
#define CONTENT_LIGHT_LEVEL_TOKEN "--content-light"
#define FGS_TABLE_TOKEN "--fgs-table"
#ifdef LIBDOVI_FOUND
#define DOLBY_VISION_RPU_TOKEN "--dolby-vision-rpu"
#endif
#ifdef LIBHDR10PLUS_RS_FOUND
#define HDR10PLUS_JSON_TOKEN "--hdr10plus-json"
#endif

#define SFRAME_DIST_TOKEN "--sframe-dist"
#define SFRAME_MODE_TOKEN "--sframe-mode"

#define ENABLE_QM_TOKEN "--enable-qm"
#define MIN_QM_LEVEL_TOKEN "--qm-min"
#define MAX_QM_LEVEL_TOKEN "--qm-max"

#define STARTUP_MG_SIZE_TOKEN "--startup-mg-size"
#define STARTUP_QP_OFFSET_TOKEN "--startup-qp-offset"
#define ROI_MAP_FILE_TOKEN "--roi-map-file"

#define ENABLE_VARIANCE_BOOST_TOKEN "--enable-variance-boost"
#define VARIANCE_BOOST_STRENGTH_TOKEN "--variance-boost-strength"
#define VARIANCE_OCTILE_TOKEN "--variance-octile"
#define TF_STRENGTH_FILTER_TOKEN "--tf-strength"
#define SHARPNESS_TOKEN "--sharpness"
#define VARIANCE_BOOST_CURVE_TOKEN "--variance-boost-curve"
#define LUMINANCE_QP_BIAS_TOKEN "--luminance-qp-bias"
#define LOSSLESS_TOKEN "--lossless"
#define AVIF_TOKEN "--avif"
#define QP_SCALE_COMPRESS_STRENGTH_TOKEN "--qp-scale-compress-strength"

#define MAX_32_TX_SIZE_TOKEN "--max-32-tx-size"

#define MIN_CHROMA_QM_LEVEL_TOKEN "--chroma-qm-min"
#define MAX_CHROMA_QM_LEVEL_TOKEN "--chroma-qm-max"

#define NOISE_NORM_STRENGTH_TOKEN "--noise-norm-strength"
#define KF_TF_STRENGTH_FILTER_TOKEN "--kf-tf-strength"

#define PSY_RD_TOKEN "--psy-rd"
#define SPY_RD_TOKEN "--spy-rd"
#define SHARP_TX_TOKEN "--sharp-tx"
#define HBD_MDS_TOKEN "--hbd-mds"

static EbErrorType validate_error(EbErrorType err, const char *token, const char *value) {
    switch (err) {
    case EB_ErrorNone: return EB_ErrorNone;
    default: fprintf(stderr, "Error: Invalid parameter '%s' with value '%s'\n", token, value); return err;
    }
}

/* copied from EbEncSettings.c */
static EbErrorType str_to_int64(const char *token, const char *nptr, int64_t *out) {
    char   *endptr;
    int64_t val;

    val = strtoll(nptr, &endptr, 0);

    if (endptr == nptr || *endptr)
        return validate_error(EB_ErrorBadParameter, token, nptr);

    *out = val;
    return EB_ErrorNone;
}

static EbErrorType str_to_int(const char *token, const char *nptr, int32_t *out) {
    char   *endptr;
    int32_t val;

    val = strtol(nptr, &endptr, 0);

    if (endptr == nptr || *endptr)
        return validate_error(EB_ErrorBadParameter, token, nptr);

    *out = val;
    return EB_ErrorNone;
}

static EbErrorType str_to_uint(const char *token, const char *nptr, uint32_t *out) {
    char    *endptr;
    uint32_t val;

    if (strtol(nptr, NULL, 0) < 0) {
        fprintf(stderr,
                "Error: Invalid parameter '%s' with value '%s'. Token unable to accept negative "
                "values\n",
                token,
                nptr);
        return EB_ErrorBadParameter;
    }

    val = strtoul(nptr, &endptr, 0);

    if (endptr == nptr || *endptr)
        return validate_error(EB_ErrorBadParameter, token, nptr);

    *out = val;
    return EB_ErrorNone;
}

static EbErrorType str_to_str(const char *nptr, char **out, const char *token) {
    (void)token;
    if (*out) {
        free(*out);
        *out = NULL;
    }
    const size_t len = strlen(nptr) + 1;
    char        *buf = (char *)malloc(len);
    if (!buf)
        return validate_error(EB_ErrorInsufficientResources, token, nptr);
    if (strcpy_s(buf, len, nptr)) {
        free(buf);
        return validate_error(EB_ErrorInsufficientResources, token, nptr);
    }
    *out = buf;
    return EB_ErrorNone;
}

#ifdef _WIN32
static HANDLE get_file_handle(FILE *fp) { return (HANDLE)_get_osfhandle(_fileno(fp)); }
#endif

static bool fopen_and_lock(FILE **file, const char *name, bool write) {
    if (!file || !name)
        return false;

    const char *mode = write ? "wb" : "rb";
    FOPEN(*file, name, mode);
    if (!*file)
        return false;

#ifdef _WIN32
    HANDLE handle = get_file_handle(*file);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    if (LockFile(handle, 0, 0, MAXDWORD, MAXDWORD))
        return true;
#else
    int fd = fileno(*file);
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return true;
#endif
    fprintf(stderr, "ERROR: locking %s failed, is it used by other encoder?\n", name);
    return false;
}

static EbErrorType open_file(FILE **file, const char *token, const char *name, const char *mode) {
    if (!file || !name)
        return validate_error(EB_ErrorBadParameter, token, "");

    if (*file) {
        fclose(*file);
        *file = NULL;
    }

    FILE *f;
    FOPEN(f, name, mode);
    if (!f)
        return validate_error(EB_ErrorBadParameter, token, name);

    *file = f;
    return EB_ErrorNone;
}

/**********************************
 * Set Cfg Functions
 **********************************/
// file options
static EbErrorType set_cfg_input_file(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    if (cfg->input_file && !cfg->input_file_is_fifo)
        fclose(cfg->input_file);

    if (!value) {
        cfg->input_file = NULL;
        return validate_error(EB_ErrorBadParameter, token, "");
    }

    if (!strcmp(value, "stdin") || !strcmp(value, "-")) {
        cfg->input_file         = stdin;
        cfg->input_file_is_fifo = true;
    } else
        FOPEN(cfg->input_file, value, "rb");

    if (cfg->input_file == NULL) {
        return validate_error(EB_ErrorBadParameter, token, value);
    }
    if (cfg->input_file != stdin) {
#ifdef _WIN32
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(cfg->input_file));
        if (handle == INVALID_HANDLE_VALUE)
            return validate_error(EB_ErrorBadParameter, token, value);
        cfg->input_file_is_fifo = GetFileType(handle) == FILE_TYPE_PIPE;
#else
        int         fd = fileno(cfg->input_file);
        struct stat statbuf;
        fstat(fd, &statbuf);
        cfg->input_file_is_fifo = S_ISFIFO(statbuf.st_mode);
#endif
    }

    cfg->y4m_input = check_if_y4m(cfg);
    return EB_ErrorNone;
}
static EbErrorType set_cfg_stream_file(EbConfig *cfg, const char *token, const char *value) {
    if (!strcmp(value, "stdout") || !strcmp(value, "-")) {
        if (cfg->bitstream_file && cfg->bitstream_file != stdout) {
            fclose(cfg->bitstream_file);
        }
        cfg->bitstream_file = stdout;
        return EB_ErrorNone;
    }
    return open_file(&cfg->bitstream_file, token, value, "wb");
}
static EbErrorType set_cfg_error_file(EbConfig *cfg, const char *token, const char *value) {
    if (!strcmp(value, "stderr")) {
        if (cfg->error_log_file && cfg->error_log_file != stderr) {
            fclose(cfg->error_log_file);
        }
        cfg->error_log_file = stderr;
        return EB_ErrorNone;
    }
    return open_file(&cfg->error_log_file, token, value, "w+");
}
static EbErrorType set_cfg_recon_file(EbConfig *cfg, const char *token, const char *value) {
    return open_file(&cfg->recon_file, token, value, "wb");
}
static EbErrorType set_cfg_qp_file(EbConfig *cfg, const char *token, const char *value) {
    return open_file(&cfg->qp_file, token, value, "r");
}
static EbErrorType set_cfg_stat_file(EbConfig *cfg, const char *token, const char *value) {
    return open_file(&cfg->stat_file, token, value, "wb");
}
static EbErrorType set_cfg_roi_map_file(EbConfig *cfg, const char *token, const char *value) {
    return open_file(&cfg->roi_map_file, token, value, "r");
}
static EbErrorType set_cfg_fgs_table_path(EbConfig *cfg, const char *token, const char *value) {
    EbErrorType ret  = EB_ErrorBadParameter;
    FILE       *file = NULL;
    if ((ret = open_file(&file, token, value, "r")) < 0)
        return ret;
    fclose(file);

    cfg->fgs_table_path = strdup(value);

    return EB_ErrorNone;
}
#ifdef LIBDOVI_FOUND
static EbErrorType set_cfg_dovi_rpu(EbConfig *cfg, const char *token, const char *value) {
    printf("Svt[info]: Parsing Dolby Vision RPU file...\n");
    const DoviRpuOpaqueList *rpus = dovi_parse_rpu_bin_file(value);
    if (rpus->error) {
        fprintf(stderr, "%s\n", rpus->error);
        dovi_rpu_list_free(rpus);
        return validate_error(EB_ErrorBadParameter, token, value);
    }
    printf("Svt[info]: Loaded %zu DoVi RPUs\n", rpus->len);
    cfg->dovi_rpus = rpus;
    return EB_ErrorNone;
}
#endif

#ifdef LIBHDR10PLUS_RS_FOUND
static EbErrorType set_cfg_hdr10plus_json(EbConfig *cfg, const char *token, const char *value) {
    printf("Svt[info]: Parsing HDR10+ JSON file...\n");
    Hdr10PlusRsJsonOpaque *hdr10plus_json = hdr10plus_rs_parse_json(value);
    const char *error = hdr10plus_rs_json_get_error(hdr10plus_json);
    if (error) {
        fprintf(stderr, "%s\n", error);
        hdr10plus_rs_json_free(hdr10plus_json);
        return validate_error(EB_ErrorBadParameter, token, value);
    }
    printf("Svt[info]: Loaded HDR10+ JSON file\n");
    cfg->hdr10plus_json = hdr10plus_json;
    return EB_ErrorNone;
}
#endif

static EbErrorType set_two_pass_stats(EbConfig *cfg, const char *token, const char *value) {
    return str_to_str(value, (char **)&cfg->stats, token);
}

static EbErrorType set_passes(EbConfig *cfg, const char *token, const char *value) {
    (void)cfg;
    (void)token;
    (void)value;
    /* empty function, we will handle passes at higher level*/
    return EB_ErrorNone;
}

static EbErrorType set_cfg_frames_to_be_encoded(EbConfig *cfg, const char *token, const char *value) {
    return str_to_int64(token, value, &cfg->frames_to_be_encoded);
}
static EbErrorType set_cfg_frames_to_be_skipped(EbConfig *cfg, const char *token, const char *value) {
    EbErrorType ret = str_to_int64(token, value, &cfg->frames_to_be_skipped);
    if (cfg->frames_to_be_skipped > 0)
        cfg->need_to_skip = true;
    return ret;
}
static EbErrorType set_buffered_input(EbConfig *cfg, const char *token, const char *value) {
    return str_to_int(token, value, &cfg->buffered_input);
}
static EbErrorType set_cfg_force_key_frames(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    struct forced_key_frames fkf;
    fkf.specifiers = NULL;
    fkf.frames     = NULL;
    fkf.count      = 0;

    if (!value)
        return EB_ErrorBadParameter;
    const char *p = value;
    while (p) {
        const size_t len       = strcspn(p, ",");
        char        *specifier = (char *)calloc(len + 1, sizeof(*specifier));
        if (!specifier)
            goto err;
        memcpy(specifier, p, len);
        char **tmp = (char **)realloc(fkf.specifiers, sizeof(*fkf.specifiers) * (fkf.count + 1));
        if (!tmp) {
            free(specifier);
            goto err;
        }
        fkf.specifiers            = tmp;
        fkf.specifiers[fkf.count] = specifier;
        fkf.count++;
        if ((p = strchr(p, ',')))
            ++p;
    }

    if (!fkf.count)
        goto err;

    fkf.frames = (uint64_t *)calloc(fkf.count, sizeof(*fkf.frames));
    if (!fkf.frames)
        goto err;
    for (size_t i = 0; i < cfg->forced_keyframes.count; ++i) free(cfg->forced_keyframes.specifiers[i]);
    free(cfg->forced_keyframes.specifiers);
    free(cfg->forced_keyframes.frames);
    cfg->forced_keyframes = fkf;
    svt_av1_enc_parse_parameter(&cfg->config, "enable-force-key-frames", "true");
    return EB_ErrorNone;
err:
    fputs("Error parsing forced key frames list\n", stderr);
    for (size_t i = 0; i < fkf.count; ++i) free(fkf.specifiers[i]);
    free(fkf.specifiers);
    return EB_ErrorBadParameter;
}
static EbErrorType set_no_progress(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    switch (value ? *value : '1') {
    case '0': cfg->progress = 1; break; // equal to --progress 1
    default: cfg->progress = 0; break; // equal to --progress 0
    }
    return EB_ErrorNone;
}
static EbErrorType set_progress(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    switch (value ? *value : '1') {
    case '0': cfg->progress = 0; break; // no progress printed
    case '2': cfg->progress = 2; break; // aomenc style progress
    case '3': cfg->progress = 3; break; // Patman style progress
    default: cfg->progress = 1; break; // default progress
    }
    return EB_ErrorNone;
}
static EbErrorType set_frame_rate(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    cfg->config.frame_rate_numerator   = strtoul(value, NULL, 0);
    cfg->config.frame_rate_denominator = 1;
    return EB_ErrorNone;
}

/**
 * @brief split colon separated string into key=value pairs
 *
 * @param[in]  str colon separated string of key=val
 * @param[out] opt key and val, both need to be freed
 */
struct ParseOpt {
    char *key;
    char *val;
};

static struct ParseOpt split_colon_keyequalval_pairs(const char **p) {
    const char     *str        = *p;
    struct ParseOpt opt        = {NULL, NULL};
    const size_t    string_len = strcspn(str, ":");

    const char *val = strchr(str, '=');
    if (!val || !*++val)
        return opt;

    const size_t key_len = val - str - 1;
    const size_t val_len = string_len - key_len - 1;
    if (!key_len || !val_len)
        return opt;

    opt.key = (char *)malloc(key_len + 1);
    opt.val = (char *)malloc(val_len + 1);
    if (!opt.key || !opt.val) {
        free(opt.key);
        opt.key = NULL;
        free(opt.val);
        opt.val = NULL;
        return opt;
    }

    memcpy(opt.key, str, key_len);
    memcpy(opt.val, val, val_len);
    opt.key[key_len] = '\0';
    opt.val[val_len] = '\0';
    str += string_len;
    if (*str)
        str++;
    *p = str;
    return opt;
}

static EbErrorType parse_svtav1_params(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    const char *p   = value;
    EbErrorType err = EB_ErrorNone;
    while (*p) {
        struct ParseOpt opt = split_colon_keyequalval_pairs(&p);
        if (!opt.key || !opt.val)
            continue;
        err = (EbErrorType)(err | svt_av1_enc_parse_parameter(&cfg->config, opt.key, opt.val));
        if (err != EB_ErrorNone) {
            fprintf(stderr, "Warning: failed to set parameter '%s' with key '%s'\n", opt.key, opt.val);
        }
        free(opt.key);
        free(opt.val);
    }
    return err;
}

static EbErrorType set_cdef_enable(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    // Set CDEF to either DEFAULT or 0
    int32_t     cdef_enable = DEFAULT;
    EbErrorType err         = str_to_int(token, value, &cdef_enable);
    cfg->config.cdef_level  = (cdef_enable == 0) ? 0 : DEFAULT;
    return err;
};

static EbErrorType set_level(EbConfig *cfg, const char *token, const char *value) {
    (void)token;
    if (strtoul(value, NULL, 0) != 0 || strcmp(value, "0") == 0)
        cfg->config.level = (uint32_t)(10 * strtod(value, NULL));
    else
        cfg->config.level = 9999999;
    return EB_ErrorNone;
};
static EbErrorType set_injector(EbConfig *cfg, const char *token, const char *value) {
    return str_to_uint(token, value, &cfg->injector);
}

static EbErrorType set_injector_frame_rate(EbConfig *cfg, const char *token, const char *value) {
    return str_to_uint(token, value, &cfg->injector_frame_rate);
}

static EbErrorType set_cfg_generic_token(EbConfig *cfg, const char *token, const char *value) {
    if (!strncmp(token, "--", 2))
        token += 2;
    if (!strncmp(token, "-", 1))
        token += 1;
    return validate_error(svt_av1_enc_parse_parameter(&cfg->config, token, value), token, value);
}

enum CfgType {
    SINGLE_INPUT, // Configuration parameters that have only 1 value input
    ARRAY_INPUT // Configuration parameters that have multiple values as input
};

/**********************************
 * Config Entry Struct
 **********************************/
typedef struct config_entry_s {
    enum CfgType type;
    const char  *token;
    const char  *name;
    EbErrorType (*scf)(EbConfig *cfg, const char *token, const char *value);
} ConfigEntry;

/**********************************
 * Config Entry Array
 **********************************/
ConfigEntry config_entry_options[] = {
    // File I/O
    {SINGLE_INPUT, HELP_TOKEN, "Shows the command line options currently available", set_cfg_input_file},
    {SINGLE_INPUT, COLORH_TOKEN, "Extra help for adding AV1 metadata to the bitstream", set_cfg_input_file},
    {SINGLE_INPUT, VERSION_TOKEN, "Shows the version of the library that's linked to the library", set_cfg_input_file},
    {SINGLE_INPUT,
     INPUT_FILE_TOKEN,
     "Input raw video (y4m and yuv) file path, use `stdin` or `-` to read from pipe",
     set_cfg_input_file},
    {SINGLE_INPUT,
     INPUT_FILE_LONG_TOKEN,
     "Input raw video (y4m and yuv) file path, use `stdin` or `-` to read from pipe",
     set_cfg_input_file},

    {SINGLE_INPUT,
     OUTPUT_BITSTREAM_TOKEN,
     "Output compressed (ivf) file path, use `stdout` or `-` to write to pipe",
     set_cfg_stream_file},
    {SINGLE_INPUT,
     OUTPUT_BITSTREAM_LONG_TOKEN,
     "Output compressed (ivf) file path, use `stdout` or `-` to write to pipe",
     set_cfg_stream_file},

    {SINGLE_INPUT, CONFIG_FILE_TOKEN, "Configuration file path", set_cfg_input_file},
    {SINGLE_INPUT, CONFIG_FILE_LONG_TOKEN, "Configuration file path", set_cfg_input_file},

    {SINGLE_INPUT, ERROR_FILE_TOKEN, "Error file path, defaults to stderr", set_cfg_error_file},
    {SINGLE_INPUT, OUTPUT_RECON_TOKEN, "Reconstructed yuv file path", set_cfg_recon_file},
    {SINGLE_INPUT, OUTPUT_RECON_LONG_TOKEN, "Reconstructed yuv file path", set_cfg_recon_file},

    {SINGLE_INPUT,
     STAT_FILE_TOKEN,
     "PSNR / SSIM per picture stat output file path, requires `--enable-stat-report 1`",
     set_cfg_stat_file},

    {SINGLE_INPUT,
     PROGRESS_TOKEN,
     "Verbosity of the output, default is 1 [0: no progress is printed, 2: aomenc style output, 3: fancy progress]",
     set_progress},
    {SINGLE_INPUT,
     NO_PROGRESS_TOKEN,
     "Do not print out progress, default is 0 [1: `" PROGRESS_TOKEN " 0`, 0: `" PROGRESS_TOKEN " 1`]",
     set_no_progress},

    {SINGLE_INPUT,
     PRESET_TOKEN,
     "Encoder preset, presets < 0 are for debugging. Higher presets means faster encodes, but with "
     "a quality tradeoff, default is 10 [-1-13]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     SVTAV1_PARAMS,
     "colon separated list of key=value pairs of parameters with keys based on config file options",
     parse_svtav1_params},

    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_global_options[] = {
    // Picture Dimensions
    {SINGLE_INPUT,
     WIDTH_TOKEN,
     "Frame width in pixels, inferred if y4m, default is 0 [4-16384]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     WIDTH_LONG_TOKEN,
     "Frame width in pixels, inferred if y4m, default is 0 [4-16384]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     HEIGHT_TOKEN,
     "Frame height in pixels, inferred if y4m, default is 0 [4-8704]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     HEIGHT_LONG_TOKEN,
     "Frame height in pixels, inferred if y4m, default is 0 [4-8704]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     FORCED_MAX_FRAME_WIDTH_TOKEN,
     "Maximum frame width value to force, default is 0 [4-16384]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     FORCED_MAX_FRAME_HEIGHT_TOKEN,
     "Maximum frame height value to force, default is 0 [4-8704]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     NUMBER_OF_PICTURES_TOKEN,
     "Number of frames to encode. If `n` is larger than the input, the encoder will loop back and "
     "continue encoding, default is 0 [0: until EOF, 1-`(2^63)-1`]",
     set_cfg_frames_to_be_encoded},
    {SINGLE_INPUT,
     NUMBER_OF_PICTURES_LONG_TOKEN,
     "Number of frames to encode. If `n` is larger than the input, the encoder will loop back and "
     "continue encoding, default is 0 [0: until EOF, 1-`(2^63)-1`]",
     set_cfg_frames_to_be_encoded},

    {SINGLE_INPUT,
     NUMBER_OF_PICTURES_TO_SKIP,
     "Number of frames to skip. Default is 0 [0: don`t skip, 1-`(2^63)-1`]",
     set_cfg_frames_to_be_skipped},

    {SINGLE_INPUT,
     BUFFERED_INPUT_TOKEN,
     "Buffer `n` input frames into memory and use them to encode, default is -1 [-1: no frames "
     "buffered, 1-`(2^31)-1`]",
     set_buffered_input},
    {SINGLE_INPUT,
     ENCODER_COLOR_FORMAT,
     "Color format, only yuv420 is supported at this time, default is 1 [0: yuv400, 1: yuv420, 2: "
     "yuv422, 3: yuv444]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     PROFILE_TOKEN,
     "Bitstream profile, default is 0 [0: main, 1: high, 2: professional]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     LEVEL_TOKEN,
     "Bitstream level, defined in A.3 of the av1 spec, default is 0 [0: autodetect from input, "
     "2.0-7.3]",
     set_level},
    {SINGLE_INPUT,
     FRAME_RATE_TOKEN,
     "Input video frame rate, integer values only, inferred if y4m, default is 60 [1-240]",
     set_frame_rate},
    {SINGLE_INPUT,
     FRAME_RATE_NUMERATOR_TOKEN,
     "Input video frame rate numerator, default is 60000 [0-2^32-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     FRAME_RATE_DENOMINATOR_TOKEN,
     "Input video frame rate denominator, default is 1000 [0-2^32-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     INPUT_DEPTH_TOKEN,
     "Input video file and output bitstream bit-depth, default is 10 [8, 10]",
     set_cfg_generic_token},
    // Latency
    {SINGLE_INPUT,
     INJECTOR_TOKEN,
     "Inject pictures to the library at defined frame rate, default is 0 [0-1]",
     set_injector},
    {SINGLE_INPUT,
     INJECTOR_FRAMERATE_TOKEN,
     "Set injector frame rate, only applicable with `--inj 1`, default is 60 [0-240]",
     set_injector_frame_rate},
    {SINGLE_INPUT,
     STAT_REPORT_NEW_TOKEN,
     "Calculates and outputs PSNR SSIM metrics at the end of encoding, default is 0 [0-1]",
     set_cfg_generic_token},

    // Asm Type
    {SINGLE_INPUT,
     ASM_TYPE_TOKEN,
#ifdef ARCH_AARCH64
     "Limit assembly instruction set, default is max [c, neon, crc32, neon_dotprod, neon_i8mm, sve, sve2, max]",
#else
     "Limit assembly instruction set, only applicable to x86, default is max [c, mmx, sse, sse2, sse3, ssse3, "
     "sse4_1, sse4_2, avx, avx2, avx512, max]",
#endif
     set_cfg_generic_token},
    {SINGLE_INPUT,
     THREAD_MGMNT,
     "Amount of parallelism to use. 0 means choose the level based on machine core count. Refer to Appendix A.1 "
     "of the user "
     "guide, default is 0 [0, 6]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     PIN_TOKEN,
     "Pin the execution to the first N cores. Refer to "
     "Appendix "
     "A.1 of the user guide, default is 0 [0, core count of the machine]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     TARGET_SOCKET,
     "Specifies which socket to run on, assumes a max of two sockets. Refer to Appendix A.1 of the "
     "user guide, default is -1 [-1, 0, -1]",
     set_cfg_generic_token},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_rc[] = {
    // Rate Control
    {SINGLE_INPUT,
     RATE_CONTROL_ENABLE_TOKEN,
     "Rate control mode, default is 0 [0: CRF or CQP (if `--aq-mode` is 0), 1: VBR, 2: CBR]",
     set_cfg_generic_token},
    {SINGLE_INPUT, QP_TOKEN, "Initial QP level value, default is 35 [1-63]", set_cfg_generic_token},
    {SINGLE_INPUT, QP_LONG_TOKEN, "Initial QP level value, default is 35 [1-63]", set_cfg_generic_token},
    {SINGLE_INPUT,
     CRF_LONG_TOKEN,
     "Constant Rate Factor value, setting this value is equal to `--rc 0 --aq-mode 2 --qp "
     "x`, default is 35 [1-70]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     TARGET_BIT_RATE_TOKEN,
     "Target Bitrate (kbps), only applicable for VBR and CBR encoding, default is 7000 [1-100000]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     MAX_BIT_RATE_TOKEN,
     "Maximum Bitrate (kbps) only applicable for CRF encoding, default is 0 [1-100000]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     USE_QP_FILE_TOKEN,
     "Overwrite the encoder default picture based QP assignments and use QP values from "
     "`--qp-file`, default is 0 [0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     QP_FILE_NEW_TOKEN,
     "Path to a file containing per picture QP value separated by newlines",
     set_cfg_qp_file},
    {SINGLE_INPUT,
     MAX_QP_TOKEN,
     "Maximum (highest) quantizer, only applicable for VBR and CBR, default is 63 [1-63]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     MIN_QP_TOKEN,
     "Minimum (lowest) quantizer, only applicable for VBR and CBR, default is 1 [1-63]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     ADAPTIVE_QP_ENABLE_NEW_TOKEN,
     "Set adaptive QP level, default is 2 [0: off, 1: variance base using AV1 segments, 2: deltaq "
     "pred efficiency]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     USE_FIXED_QINDEX_OFFSETS_TOKEN,
     "Overwrite the encoder default hierarchical layer based QP assignment and use fixed Q index "
     "offsets, default is 0 [0-2]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     KEY_FRAME_QINDEX_OFFSET_TOKEN,
     "Overwrite the encoder default keyframe Q index assignment, default is 0 [-256-255]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     KEY_FRAME_CHROMA_QINDEX_OFFSET_TOKEN,
     "Overwrite the encoder default chroma keyframe Q index assignment, default is 0 [-256-255]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     QINDEX_OFFSETS_TOKEN,
     "list of luma Q index offsets per hierarchical layer, separated by `,` with each offset in "
     "the range of [-256-255], default is `0,0,..,0`",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     CHROMA_QINDEX_OFFSETS_TOKEN,
     "list of chroma Q index offsets per hierarchical layer, separated by `,` with each offset in "
     "the range of [-256-255], default is `0,0,..,0`",
     set_cfg_generic_token},
    {SINGLE_INPUT, LUMA_Y_DC_QINDEX_OFFSET_TOKEN, "Luma Y DC Qindex Offset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_U_DC_QINDEX_OFFSET_TOKEN, "Chroma U DC Qindex Offset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_U_AC_QINDEX_OFFSET_TOKEN, "Chroma U AC Qindex Offset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_V_DC_QINDEX_OFFSET_TOKEN, "Chroma V DC Qindex Offset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_V_AC_QINDEX_OFFSET_TOKEN, "Chroma V AC Qindex Offset", set_cfg_generic_token},
    {SINGLE_INPUT,
     LAMBDA_SCALE_FACTORS_TOKEN,
     "list of scale factor for lambda values used for different frame types defined by SvtAv1FrameUpdateType, separated by `,` \
      with each scale factor as integer. \
      value divided by 128 is the actual scale factor in float, default is `128,128,..,128`",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     UNDER_SHOOT_PCT_TOKEN,
     "Only for VBR and CBR, allowable datarate undershoot (min) target (percentage), default is "
     "25, but can change based on rate control [0-100]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     OVER_SHOOT_PCT_TOKEN,
     "Only for VBR and CBR, allowable datarate overshoot (max) target (percentage), default is 25, "
     "but can change based on rate control [0-100]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     MBR_OVER_SHOOT_PCT_TOKEN,
     "Only for Capped CRF, allowable datarate overshoot (max) target (percentage), default is 50, "
     "but can change based on rate control [0-100]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     GOP_CONSTRAINT_RC_TOKEN,
     "Enable GoP constraint rc.  When enabled, the rate control matches the target rate for each "
     "GoP, default is 0 [0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     BUFFER_SIZE_TOKEN,
     "Client buffer size (ms), only applicable for CBR, default is 6000 [0-10000]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     BUFFER_INITIAL_SIZE_TOKEN,
     "Client initial buffer size (ms), only applicable for CBR, default is 4000 [0-10000]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     BUFFER_OPTIMAL_SIZE_TOKEN,
     "Client optimal buffer size (ms), only applicable for CBR, default is 5000 [0-10000]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     RECODE_LOOP_TOKEN,
     "Recode loop level, refer to \"Recode loop level table\" in the user guide for more info [0: "
     "off, 4: preset based]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     VBR_MIN_SECTION_PCT_TOKEN,
     "GOP min bitrate (expressed as a percentage of the target rate), default is 0 [0-100]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     VBR_MAX_SECTION_PCT_TOKEN,
     "GOP max bitrate (expressed as a percentage of the target rate), default is 2000 [0-10000]",
     set_cfg_generic_token},
    {SINGLE_INPUT, ENABLE_QM_TOKEN, "Enable quantisation matrices, default is 1 [0-1]", set_cfg_generic_token},
    {SINGLE_INPUT, MIN_QM_LEVEL_TOKEN, "Min quant matrix flatness, default is 0 [0-15]", set_cfg_generic_token},
    {SINGLE_INPUT, MAX_QM_LEVEL_TOKEN, "Max quant matrix flatness, default is 15 [0-15]", set_cfg_generic_token},
    {SINGLE_INPUT,
     ROI_MAP_FILE_TOKEN,
     "Enable Region Of Interest and specify a picture based QP Offset map file, default is off",
     set_cfg_roi_map_file},
    // TF Strength
    {SINGLE_INPUT,
     TF_STRENGTH_FILTER_TOKEN,
     "[PSY] Adjust alt-ref temporal filtering strength, default is 1 (4x weaker than mainline) [0-4]",
     set_cfg_generic_token},
    // Frame-level luminance-based QP bias
    {SINGLE_INPUT,
     LUMINANCE_QP_BIAS_TOKEN,
     "Adjusts a frame's QP based on its average luma value, default is 0 [0-100]",
     set_cfg_generic_token},
    // Sharpness
    {SINGLE_INPUT,
     SHARPNESS_TOKEN,
     "Bias towards decreased/increased sharpness, default is 1 [0 to 7]",
     set_cfg_generic_token},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_2p[] = {
    // 2 pass
    {SINGLE_INPUT,
     PASS_TOKEN,
     "Multi-pass selection, pass 2 is only available for VBR, default is 0 [0: single pass encode, "
     "1: first pass, 2: second pass]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     TWO_PASS_STATS_TOKEN,
     "Filename for multi-pass encoding, default is \"svtav1_2pass.log\"",
     set_two_pass_stats},
    {SINGLE_INPUT,
     PASSES_TOKEN,
     "Number of encoding passes, default is preset dependent but generally 1 [1: one pass encode, "
     "2: multi-pass encode]",
     set_passes},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_intra_refresh[] = {
    {SINGLE_INPUT,
     KEYINT_TOKEN,
     "GOP size (frames), default is -2 [-2: ~10 seconds, -1: \"infinite\" and only applicable for "
     "CRF, 0: same as -1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     INTRA_REFRESH_TYPE_TOKEN,
     "Intra refresh type, default is 2 [1: FWD Frame (Open GOP), 2: KEY Frame (Closed GOP)]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     SCENE_CHANGE_DETECTION_TOKEN,
     "Scene change detection control, default is 0 [0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     LOOKAHEAD_NEW_TOKEN,
     "Number of frames in the future to look ahead, not including minigop, temporal filtering, and "
     "rate "
     "control, default is -1 [-1: auto, 0-120]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     HIERARCHICAL_LEVELS_TOKEN,
     "Set hierarchical levels beyond the base layer, default is <=M12: 5, else: 4 [2: 3 temporal "
     "layers, 3: 4 "
     "temporal layers, 4: 5 "
     "layers, 5: 6 layers]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     PRED_STRUCT_TOKEN,
     "Set prediction structure, default is 2 [1: low delay frames, 2: "
     "random access]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     FORCE_KEY_FRAMES_TOKEN,
     "Force key frames at the comma separated specifiers. `#f` for frames, `#.#s` for seconds",
     set_cfg_force_key_frames},
    {SINGLE_INPUT,
     STARTUP_MG_SIZE_TOKEN,
     "Specify another mini-gop configuration for the first mini-gop after the key-frame, default "
     "is 0 [0: OFF, "
     "2: 3 temporal layers, 3: 4 temporal layers, 4: 5 temporal layers]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     STARTUP_QP_OFFSET_TOKEN,
     "Specify an offset to the input-qp of the startup GOP prior to the picture-qp derivation, default "
     "is 0 [-63,63]",
     set_cfg_generic_token},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_specific[] = {
    {SINGLE_INPUT,
     TILE_ROW_TOKEN,
     "Number of tile rows to use, `TileRow == log2(x)`, default changes per resolution but is 1 "
     "[0-6]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     TILE_COL_TOKEN,
     "Number of tile columns to use, `TileCol == log2(x)`, default changes per resolution but is 1 "
     "[0-4]",
     set_cfg_generic_token},

    // DLF
    {SINGLE_INPUT, LOOP_FILTER_ENABLE, "Deblocking loop filter control, default is 1 [0-2]", set_cfg_generic_token},
    // CDEF
    {SINGLE_INPUT,
     CDEF_ENABLE_TOKEN,
     "Enable Constrained Directional Enhancement Filter, default is 1 [0-1]",
     set_cdef_enable},
    // RESTORATION
    {SINGLE_INPUT,
     ENABLE_RESTORATION_TOKEN,
     "Enable loop restoration filter, default is 1 [0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     ENABLE_TPL_LA_TOKEN,
     "Temporal Dependency model control, currently forced on library side, only applicable for "
     "CRF/CQP, default is 1 [0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     MFMV_ENABLE_NEW_TOKEN,
     "Motion Field Motion Vector control, default is -1 [-1: auto, 0-1]",
     set_cfg_generic_token},
    {SINGLE_INPUT, DG_ENABLE_NEW_TOKEN, "Dynamic GoP control, default is 1 [0-1]", set_cfg_generic_token},
    {SINGLE_INPUT, FAST_DECODE_TOKEN, "Fast Decoder levels, default is 0 [0-2]", set_cfg_generic_token},
    // --- start: ALTREF_FILTERING_SUPPORT
    {SINGLE_INPUT,
     ENABLE_TF_TOKEN,
     "Enable ALT-REF (temporally filtered) frames, default is 1 [0-2]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     ENABLE_OVERLAYS,
     "Enable the insertion of overlayer pictures which will be used as an additional reference "
     "frame for the base layer picture, default is 0 [0-1]",
     set_cfg_generic_token},
    // --- end: ALTREF_FILTERING_SUPPORT
    {SINGLE_INPUT,
     TUNE_TOKEN,
     "Optimize the encoding process for different desired outcomes [0 = VQ, 1 = PSNR, 2 = SSIM, 3 = Subjective SSIM, 4 = Still Picture], "
     "default is 2 "
     "[0-4]",
     set_cfg_generic_token},
    // MD Parameters
    {SINGLE_INPUT,
     SCREEN_CONTENT_TOKEN,
     "Set screen content detection level, default is 2 [0: off, 1: on, 2: content adaptive]",
     set_cfg_generic_token},
    // Annex A parameters
    {SINGLE_INPUT,
     FILM_GRAIN_TOKEN,
     "Enable film grain, default is 0 [0: off, 1-50: level of denoising for film grain]",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     FILM_GRAIN_DENOISE_APPLY_TOKEN,
     "Apply denoising when film grain is ON, default is 0 [0: no denoising, film grain data is "
     "still in frame header, "
     "1: level of denoising is set by the film-grain parameter]",
     set_cfg_generic_token},

    {SINGLE_INPUT, FGS_TABLE_TOKEN, "Set the film grain model table path", set_cfg_fgs_table_path},

    // --- start: SUPER-RESOLUTION SUPPORT
    {SINGLE_INPUT,
     SUPERRES_MODE_INPUT,
     "Enable super-resolution mode, refer to the super-resolution section in the user guide, "
     "default is 0 [0: off, 1-3, 4: auto-select mode]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     SUPERRES_DENOM,
     "Super-resolution denominator, only applicable for mode == 1, default is 8 [8: no scaling, "
     "9-15, 16: half-scaling]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     SUPERRES_KF_DENOM,
     "Super-resolution denominator for key frames, only applicable for mode == 1, default is 8 [8: "
     "no scaling, 9-15, 16: half-scaling]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     SUPERRES_QTHRES,
     "Super-resolution q-threshold, only applicable for mode == 3, default is 43 [0-63]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     SUPERRES_KF_QTHRES,
     "Super-resolution q-threshold for key frames, only applicable for mode == 3, default is 43 "
     "[0-63]",
     set_cfg_generic_token},
    // --- end: SUPER-RESOLUTION SUPPORT

    // --- start: SWITCH_FRAME SUPPORT
    {SINGLE_INPUT, SFRAME_DIST_TOKEN, "S-Frame interval (frames) (0: OFF[default], > 0: ON)", set_cfg_generic_token},
    {SINGLE_INPUT,
     SFRAME_MODE_TOKEN,
     "S-Frame insertion mode ([1-2], 1: the considered frame will be made into an S-Frame only if "
     "it is an altref frame,"
     " 2: the next altref frame will be made into an S-Frame[default])",
     set_cfg_generic_token},
    // --- end: SWITCH_FRAME SUPPORT
    // --- start: REFERENCE SCALING SUPPORT
    {SINGLE_INPUT,
     RESIZE_MODE_INPUT,
     "Enable resize mode [0: none, 1: fixed scale, 2: random scale, 3: dynamic scale, 4: random "
     "access]",
     set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_DENOM, "Resize denominator, only applicable for mode == 1 [8-16]", set_cfg_generic_token},
    {SINGLE_INPUT,
     RESIZE_KF_DENOM,
     "Resize denominator for key frames, only applicable for mode == 1 [8-16]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     RESIZE_FRAME_EVTS,
     "Resize frame events, in a list separated by ',', a reference scaling process starts from the "
     "given frame number with new denominators, only applicable for mode == 4",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     RESIZE_FRAME_KF_DENOMS,
     "Resize denominator for key frames in event, in a list separated by ',', only applicable for "
     "mode == 4",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     RESIZE_FRAME_DENOMS,
     "Resize denominator in event, in a list separated by ',', only applicable for mode == 4",
     set_cfg_generic_token},
    // --- end: REFERENCE SCALING SUPPORT
    {SINGLE_INPUT, LOSSLESS_TOKEN, "Enable lossless coding, default is 0 [0-1]", set_cfg_generic_token},
    {SINGLE_INPUT, AVIF_TOKEN, "Enable still-picture coding, default is 0 [0-1]", set_cfg_generic_token},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_color_description[] = {
    // Color description help
    {SINGLE_INPUT, COLORH_TOKEN, "[PSY] Metadata help from user guide Appendix A.2", set_cfg_generic_token},
    // Color description
    {SINGLE_INPUT,
     COLOR_PRIMARIES_NEW_TOKEN,
     "Color primaries, refer to --color-help. Default is 2 [0-12, 22]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     TRANSFER_CHARACTERISTICS_NEW_TOKEN,
     "Transfer characteristics, refer to --color-help. Default is 2 [0-22]",
     set_cfg_generic_token},
    {SINGLE_INPUT,
     MATRIX_COEFFICIENTS_NEW_TOKEN,
     "Matrix coefficients, refer to --color-help. Default is 2 [0-14]",
     set_cfg_generic_token},
    {SINGLE_INPUT, COLOR_RANGE_NEW_TOKEN, "Color range, default is 0 [0: Studio, 1: Full]", set_cfg_generic_token},
    {SINGLE_INPUT,
     CHROMA_SAMPLE_POSITION_TOKEN,
     "Chroma sample position, default is 'unknown' ['unknown', 'vertical'/'left', "
     "'colocated'/'topleft']",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     MASTERING_DISPLAY_TOKEN,
     "Mastering display metadata in the format of \"G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)\", refer "
     "to the user guide Appendix A.2",
     set_cfg_generic_token},

    {SINGLE_INPUT,
     CONTENT_LIGHT_LEVEL_TOKEN,
     "Set content light level in the format of \"max_cll,max_fall\", refer to the user guide "
     "Appendix A.2",
     set_cfg_generic_token},
    // Dolby Vision RPU
#ifdef LIBDOVI_FOUND
    {SINGLE_INPUT,
     DOLBY_VISION_RPU_TOKEN,
     "[PSY] Set the Dolby Vision RPU path",
     set_cfg_dovi_rpu},
#endif
#ifdef LIBHDR10PLUS_RS_FOUND
    {SINGLE_INPUT,
     HDR10PLUS_JSON_TOKEN,
     "[PSY] Set the HDR10+ JSON file path",
     set_cfg_hdr10plus_json},
#endif
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry_variance_boost[] = {
    // Variance boost
    {SINGLE_INPUT, ENABLE_VARIANCE_BOOST_TOKEN, "Enable variance boost, default is 0 [0-1]", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_BOOST_STRENGTH_TOKEN, "Variance boost strength, default is 2 [1-4]", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_OCTILE_TOKEN, "Octile for variance boost, default is 6 [1-8]", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_BOOST_CURVE_TOKEN, "Curve for variance boost, default is 0 [0-2]", set_cfg_generic_token},
    // QP scale compress
    {SINGLE_INPUT, QP_SCALE_COMPRESS_STRENGTH_TOKEN, "[PSY] QP scale compress strength, default is 1 [0-3]", set_cfg_generic_token},
    // Max 32 tx size
    {SINGLE_INPUT, MAX_32_TX_SIZE_TOKEN, "[PSY] Limits the allowed transform sizes to a maximum of 32x32, default is 0 [0-1]", set_cfg_generic_token},
    // Min/max chroma qm
    {SINGLE_INPUT, MIN_CHROMA_QM_LEVEL_TOKEN, "[PSY] Min chroma quant matrix flatness, default is 8 [0-15]", set_cfg_generic_token},
    {SINGLE_INPUT, MAX_CHROMA_QM_LEVEL_TOKEN, "[PSY] Max chroma quant matrix flatness, default is 15 [0-15]", set_cfg_generic_token},
    //Noise normalisation strength
    {SINGLE_INPUT, NOISE_NORM_STRENGTH_TOKEN, "[PSY] Noise normalization strength, default is 0, default for tune=3 is 3 [0-4]", set_cfg_generic_token},
    //Alt-ref temporal filtering strength on keyframes
    {SINGLE_INPUT, KF_TF_STRENGTH_FILTER_TOKEN, "[PSY] Adjust alt-ref TF strength on keyframes, default is 1 (4x weaker than mainline) [0-4]", set_cfg_generic_token},
    //Psy-rd
    {SINGLE_INPUT, PSY_RD_TOKEN, "[PSY] Psychovisual rate distortion strength, default is 0.5 [0.0-6.0]", set_cfg_generic_token},
    //Spy-rd
    {SINGLE_INPUT, SPY_RD_TOKEN, "[PSY] Alternative psychovisual rate distortion, default is 0 [0-1]", set_cfg_generic_token},
    //Sharp-tx
    {SINGLE_INPUT, SHARP_TX_TOKEN, "[PSY] Sharp transform optimization, default is 1; best used in combination with psy-rd [0-1]", set_cfg_generic_token},
    //HBD Mode Decisions
    {SINGLE_INPUT, HBD_MDS_TOKEN, "[PSY] High Bit-Depth Mode Decision, default is 0 [0: default preset behavior, 1 = 10-bit, 2 = hybrid 8/10-bit, 3 = 8-bit]", set_cfg_generic_token},
    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

ConfigEntry config_entry[] = {
    // Options
    {SINGLE_INPUT, INPUT_FILE_TOKEN, "InputFile", set_cfg_input_file},
    {SINGLE_INPUT, INPUT_FILE_LONG_TOKEN, "InputFile", set_cfg_input_file},
    {SINGLE_INPUT, OUTPUT_BITSTREAM_TOKEN, "StreamFile", set_cfg_stream_file},
    {SINGLE_INPUT, OUTPUT_BITSTREAM_LONG_TOKEN, "StreamFile", set_cfg_stream_file},
    {SINGLE_INPUT, ERROR_FILE_TOKEN, "ErrorFile", set_cfg_error_file},
    {SINGLE_INPUT, OUTPUT_RECON_TOKEN, "ReconFile", set_cfg_recon_file},
    {SINGLE_INPUT, OUTPUT_RECON_LONG_TOKEN, "ReconFile", set_cfg_recon_file},
    {SINGLE_INPUT, STAT_FILE_TOKEN, "StatFile", set_cfg_stat_file},
    {SINGLE_INPUT, PROGRESS_TOKEN, "Progress", set_progress},
    {SINGLE_INPUT, NO_PROGRESS_TOKEN, "NoProgress", set_no_progress},
    {SINGLE_INPUT, PRESET_TOKEN, "EncoderMode", set_cfg_generic_token},
    {SINGLE_INPUT, SVTAV1_PARAMS, "SvtAv1Params", parse_svtav1_params},

    // Encoder Global Options
    //   Picture Dimensions
    {SINGLE_INPUT, WIDTH_TOKEN, "SourceWidth", set_cfg_generic_token},
    {SINGLE_INPUT, WIDTH_LONG_TOKEN, "SourceWidth", set_cfg_generic_token},
    {SINGLE_INPUT, HEIGHT_TOKEN, "SourceHeight", set_cfg_generic_token},
    {SINGLE_INPUT, HEIGHT_LONG_TOKEN, "SourceHeight", set_cfg_generic_token},
    {SINGLE_INPUT, FORCED_MAX_FRAME_WIDTH_TOKEN, "ForcedMaximumFrameWidth", set_cfg_generic_token},
    {SINGLE_INPUT, FORCED_MAX_FRAME_HEIGHT_TOKEN, "ForcedMaximumFrameHeight", set_cfg_generic_token},
    // Prediction Structure
    {SINGLE_INPUT, NUMBER_OF_PICTURES_TOKEN, "FrameToBeEncoded", set_cfg_frames_to_be_encoded},
    {SINGLE_INPUT, NUMBER_OF_PICTURES_LONG_TOKEN, "FrameToBeEncoded", set_cfg_frames_to_be_encoded},
    {SINGLE_INPUT, BUFFERED_INPUT_TOKEN, "BufferedInput", set_buffered_input},

    {SINGLE_INPUT, NUMBER_OF_PICTURES_TO_SKIP, "FrameToBeSkipped", set_cfg_frames_to_be_skipped},

    //   Annex A parameters
    {SINGLE_INPUT, TIER_TOKEN, "Tier", set_cfg_generic_token}, // Lacks a command line flag for now
    {SINGLE_INPUT, ENCODER_COLOR_FORMAT, "EncoderColorFormat", set_cfg_generic_token},
    {SINGLE_INPUT, PROFILE_TOKEN, "Profile", set_cfg_generic_token},
    {SINGLE_INPUT, LEVEL_TOKEN, "Level", set_level},
    //   Frame Rate tokens
    {SINGLE_INPUT, FRAME_RATE_TOKEN, "FrameRate", set_frame_rate},
    {SINGLE_INPUT, FRAME_RATE_NUMERATOR_TOKEN, "FrameRateNumerator", set_cfg_generic_token},
    {SINGLE_INPUT, FRAME_RATE_DENOMINATOR_TOKEN, "FrameRateDenominator", set_cfg_generic_token},

    //   Bit depth tokens
    {SINGLE_INPUT, INPUT_DEPTH_TOKEN, "EncoderBitDepth", set_cfg_generic_token},

    //   Latency
    {SINGLE_INPUT, INJECTOR_TOKEN, "Injector", set_injector},
    {SINGLE_INPUT, INJECTOR_FRAMERATE_TOKEN, "InjectorFrameRate", set_injector_frame_rate},

    {SINGLE_INPUT, STAT_REPORT_NEW_TOKEN, "StatReport", set_cfg_generic_token},

    //   Asm Type
    {SINGLE_INPUT, ASM_TYPE_TOKEN, "Asm", set_cfg_generic_token},

    //   Thread Management
    {SINGLE_INPUT, THREAD_MGMNT, "LevelOfParallelism", set_cfg_generic_token},
    {SINGLE_INPUT, PIN_TOKEN, "PinnedExecution", set_cfg_generic_token},
    {SINGLE_INPUT, TARGET_SOCKET, "TargetSocket", set_cfg_generic_token},

    // Rate Control Options
    {SINGLE_INPUT, RATE_CONTROL_ENABLE_TOKEN, "RateControlMode", set_cfg_generic_token},
    {SINGLE_INPUT, QP_TOKEN, "QP", set_cfg_generic_token},
    {SINGLE_INPUT, QP_LONG_TOKEN, "QP", set_cfg_generic_token},
    {SINGLE_INPUT, CRF_LONG_TOKEN, "CRF", set_cfg_generic_token},
    {SINGLE_INPUT, TARGET_BIT_RATE_TOKEN, "TargetBitRate", set_cfg_generic_token},
    {SINGLE_INPUT, MAX_BIT_RATE_TOKEN, "MaxBitRate", set_cfg_generic_token},

    {SINGLE_INPUT, USE_QP_FILE_TOKEN, "UseQpFile", set_cfg_generic_token},
    {SINGLE_INPUT, QP_FILE_NEW_TOKEN, "QpFile", set_cfg_qp_file},

    {SINGLE_INPUT, MAX_QP_TOKEN, "MaxQpAllowed", set_cfg_generic_token},
    {SINGLE_INPUT, MIN_QP_TOKEN, "MinQpAllowed", set_cfg_generic_token},

    {SINGLE_INPUT, ADAPTIVE_QP_ENABLE_NEW_TOKEN, "AdaptiveQuantization", set_cfg_generic_token},

    //   qindex offsets
    {SINGLE_INPUT, USE_FIXED_QINDEX_OFFSETS_TOKEN, "UseFixedQIndexOffsets", set_cfg_generic_token},
    {SINGLE_INPUT, KEY_FRAME_QINDEX_OFFSET_TOKEN, "KeyFrameQIndexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, KEY_FRAME_CHROMA_QINDEX_OFFSET_TOKEN, "KeyFrameChromaQIndexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, QINDEX_OFFSETS_TOKEN, "QIndexOffsets", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_QINDEX_OFFSETS_TOKEN, "ChromaQIndexOffsets", set_cfg_generic_token},
    {SINGLE_INPUT, LUMA_Y_DC_QINDEX_OFFSET_TOKEN, "LumaYDCQindexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_U_DC_QINDEX_OFFSET_TOKEN, "ChromaUDCQindexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_U_AC_QINDEX_OFFSET_TOKEN, "ChromaUACQindexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_V_DC_QINDEX_OFFSET_TOKEN, "ChromaVDCQindexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_V_AC_QINDEX_OFFSET_TOKEN, "ChromaVACQindexOffset", set_cfg_generic_token},
    {SINGLE_INPUT, LAMBDA_SCALE_FACTORS_TOKEN, "LambdaScaleFactors", set_cfg_generic_token},
    {SINGLE_INPUT, UNDER_SHOOT_PCT_TOKEN, "UnderShootPct", set_cfg_generic_token},
    {SINGLE_INPUT, OVER_SHOOT_PCT_TOKEN, "OverShootPct", set_cfg_generic_token},
    {SINGLE_INPUT, MBR_OVER_SHOOT_PCT_TOKEN, "MbrOverShootPct", set_cfg_generic_token},
    {SINGLE_INPUT, GOP_CONSTRAINT_RC_TOKEN, "GopConstraintRc", set_cfg_generic_token},
    {SINGLE_INPUT, BUFFER_SIZE_TOKEN, "BufSz", set_cfg_generic_token},
    {SINGLE_INPUT, BUFFER_INITIAL_SIZE_TOKEN, "BufInitialSz", set_cfg_generic_token},
    {SINGLE_INPUT, BUFFER_OPTIMAL_SIZE_TOKEN, "BufOptimalSz", set_cfg_generic_token},
    {SINGLE_INPUT, RECODE_LOOP_TOKEN, "RecodeLoop", set_cfg_generic_token},
    {SINGLE_INPUT, VBR_MIN_SECTION_PCT_TOKEN, "MinSectionPct", set_cfg_generic_token},
    {SINGLE_INPUT, VBR_MAX_SECTION_PCT_TOKEN, "MaxSectionPct", set_cfg_generic_token},

    // Multi-pass Options
    {SINGLE_INPUT, PASS_TOKEN, "Pass", set_cfg_generic_token},
    {SINGLE_INPUT, TWO_PASS_STATS_TOKEN, "Stats", set_two_pass_stats},
    {SINGLE_INPUT, PASSES_TOKEN, "Passes", set_passes},

    // GOP size and type Options
    {SINGLE_INPUT, INTRA_PERIOD_TOKEN, "IntraPeriod", set_cfg_generic_token},
    {SINGLE_INPUT, KEYINT_TOKEN, "Keyint", set_cfg_generic_token},
    {SINGLE_INPUT, INTRA_REFRESH_TYPE_TOKEN, "IntraRefreshType", set_cfg_generic_token},
    {SINGLE_INPUT, SCENE_CHANGE_DETECTION_TOKEN, "SceneChangeDetection", set_cfg_generic_token},
    {SINGLE_INPUT, LOOKAHEAD_NEW_TOKEN, "Lookahead", set_cfg_generic_token},
    //   Prediction Structure
    {SINGLE_INPUT, HIERARCHICAL_LEVELS_TOKEN, "HierarchicalLevels", set_cfg_generic_token},
    {SINGLE_INPUT, PRED_STRUCT_TOKEN, "PredStructure", set_cfg_generic_token},
    {SINGLE_INPUT, FORCE_KEY_FRAMES_TOKEN, "ForceKeyFrames", set_cfg_force_key_frames},
    {SINGLE_INPUT, STARTUP_MG_SIZE_TOKEN, "StartupMgSize", set_cfg_generic_token},
    {SINGLE_INPUT, STARTUP_QP_OFFSET_TOKEN, "StartupGopQpOffset", set_cfg_generic_token},
    // AV1 Specific Options
    {SINGLE_INPUT, TILE_ROW_TOKEN, "TileRow", set_cfg_generic_token},
    {SINGLE_INPUT, TILE_COL_TOKEN, "TileCol", set_cfg_generic_token},
    {SINGLE_INPUT, LOOP_FILTER_ENABLE, "LoopFilterEnable", set_cfg_generic_token},
    {SINGLE_INPUT, CDEF_ENABLE_TOKEN, "CDEFLevel", set_cdef_enable},
    {SINGLE_INPUT, ENABLE_RESTORATION_TOKEN, "EnableRestoration", set_cfg_generic_token},
    {SINGLE_INPUT, ENABLE_TPL_LA_TOKEN, "EnableTPLModel", set_cfg_generic_token},
    {SINGLE_INPUT, MFMV_ENABLE_NEW_TOKEN, "Mfmv", set_cfg_generic_token},
    {SINGLE_INPUT, DG_ENABLE_NEW_TOKEN, "EnableDg", set_cfg_generic_token},
    {SINGLE_INPUT, FAST_DECODE_TOKEN, "FastDecode", set_cfg_generic_token},
    {SINGLE_INPUT, TUNE_TOKEN, "Tune", set_cfg_generic_token},
    //   ALT-REF filtering support
    {SINGLE_INPUT, ENABLE_TF_TOKEN, "EnableTf", set_cfg_generic_token},
    {SINGLE_INPUT, ENABLE_OVERLAYS, "EnableOverlays", set_cfg_generic_token},
    {SINGLE_INPUT, SCREEN_CONTENT_TOKEN, "ScreenContentMode", set_cfg_generic_token},
    {SINGLE_INPUT, FILM_GRAIN_TOKEN, "FilmGrain", set_cfg_generic_token},
    {SINGLE_INPUT, FILM_GRAIN_DENOISE_APPLY_TOKEN, "FilmGrainDenoise", set_cfg_generic_token},
    {SINGLE_INPUT, FGS_TABLE_TOKEN, "FilmGrainTable", set_cfg_fgs_table_path},

    //   Super-resolution support
    {SINGLE_INPUT, SUPERRES_MODE_INPUT, "SuperresMode", set_cfg_generic_token},
    {SINGLE_INPUT, SUPERRES_DENOM, "SuperresDenom", set_cfg_generic_token},
    {SINGLE_INPUT, SUPERRES_KF_DENOM, "SuperresKfDenom", set_cfg_generic_token},
    {SINGLE_INPUT, SUPERRES_QTHRES, "SuperresQthres", set_cfg_generic_token},
    {SINGLE_INPUT, SUPERRES_KF_QTHRES, "SuperresKfQthres", set_cfg_generic_token},

    // Switch frame support
    {SINGLE_INPUT, SFRAME_DIST_TOKEN, "SframeInterval", set_cfg_generic_token},
    {SINGLE_INPUT, SFRAME_MODE_TOKEN, "SframeMode", set_cfg_generic_token},
    // Reference Scaling support
    {SINGLE_INPUT, RESIZE_MODE_INPUT, "ResizeMode", set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_DENOM, "ResizeDenom", set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_KF_DENOM, "ResizeKfDenom", set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_FRAME_EVTS, "ResizeFrameEvts", set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_FRAME_KF_DENOMS, "ResizeFrameKFDenoms", set_cfg_generic_token},
    {SINGLE_INPUT, RESIZE_FRAME_DENOMS, "ResizeFrameDenoms", set_cfg_generic_token},

    // Color Description Options
    {SINGLE_INPUT, COLOR_PRIMARIES_NEW_TOKEN, "ColorPrimaries", set_cfg_generic_token},
    {SINGLE_INPUT, TRANSFER_CHARACTERISTICS_NEW_TOKEN, "TransferCharacteristics", set_cfg_generic_token},
    {SINGLE_INPUT, MATRIX_COEFFICIENTS_NEW_TOKEN, "MatrixCoefficients", set_cfg_generic_token},
    {SINGLE_INPUT, COLOR_RANGE_NEW_TOKEN, "ColorRange", set_cfg_generic_token},
    {SINGLE_INPUT, CHROMA_SAMPLE_POSITION_TOKEN, "ChromaSamplePosition", set_cfg_generic_token},
    {SINGLE_INPUT, MASTERING_DISPLAY_TOKEN, "MasteringDisplay", set_cfg_generic_token},
    {SINGLE_INPUT, CONTENT_LIGHT_LEVEL_TOKEN, "ContentLightLevel", set_cfg_generic_token},
#ifdef LIBDOVI_FOUND
    {SINGLE_INPUT, DOLBY_VISION_RPU_TOKEN, "DolbyVisionRpu", set_cfg_dovi_rpu},
#endif
#ifdef LIBHDR10PLUS_RS_FOUND
    {SINGLE_INPUT, HDR10PLUS_JSON_TOKEN, "Hdr10PlusJson", set_cfg_hdr10plus_json},
#endif

    // QM
    {SINGLE_INPUT, ENABLE_QM_TOKEN, "EnableQM", set_cfg_generic_token},
    {SINGLE_INPUT, MIN_QM_LEVEL_TOKEN, "MinQmLevel", set_cfg_generic_token},
    {SINGLE_INPUT, MAX_QM_LEVEL_TOKEN, "MaxQmLevel", set_cfg_generic_token},

    // ROI
    {SINGLE_INPUT, ROI_MAP_FILE_TOKEN, "RoiMapFile", set_cfg_roi_map_file},

    // Sharpness
    {SINGLE_INPUT, SHARPNESS_TOKEN, "Sharpness", set_cfg_generic_token},

    // Variance boost
    {SINGLE_INPUT, ENABLE_VARIANCE_BOOST_TOKEN, "EnableVarianceBoost", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_BOOST_STRENGTH_TOKEN, "VarianceBoostStrength", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_OCTILE_TOKEN, "VarianceOctile", set_cfg_generic_token},
    {SINGLE_INPUT, VARIANCE_BOOST_CURVE_TOKEN, "VarianceBoostCurve", set_cfg_generic_token},

    // TF Strength
    {SINGLE_INPUT, TF_STRENGTH_FILTER_TOKEN, "TemporalFilteringStrength", set_cfg_generic_token},

    // Frame-level luminance-based QP bias
    {SINGLE_INPUT, LUMINANCE_QP_BIAS_TOKEN, "LuminanceQpBias", set_cfg_generic_token},

    // Lossless coding
    {SINGLE_INPUT, LOSSLESS_TOKEN, "Lossless", set_cfg_generic_token},
    {SINGLE_INPUT, AVIF_TOKEN, "Avif", set_cfg_generic_token},
    // QP scale compression
    {SINGLE_INPUT, QP_SCALE_COMPRESS_STRENGTH_TOKEN, "QpScaleCompressStrength", set_cfg_generic_token},

     // Max 32 tx size
    {SINGLE_INPUT, MAX_32_TX_SIZE_TOKEN, "Max32TxSize", set_cfg_generic_token},

    // Chroma QM
    {SINGLE_INPUT, MIN_CHROMA_QM_LEVEL_TOKEN, "MinChromaQmLevel", set_cfg_generic_token},
    {SINGLE_INPUT, MAX_CHROMA_QM_LEVEL_TOKEN, "MaxChromaQmLevel", set_cfg_generic_token},

    // Noise normalization strength
    {SINGLE_INPUT, NOISE_NORM_STRENGTH_TOKEN, "NoiseNormStrength", set_cfg_generic_token},

    //Alt-ref temporal filtering strength on keyframes
    {SINGLE_INPUT, KF_TF_STRENGTH_FILTER_TOKEN, "KeyframeTemporalFilteringStrength", set_cfg_generic_token},

    // Psy rd strength
    {SINGLE_INPUT, PSY_RD_TOKEN, "PsyRd", set_cfg_generic_token},

    // Spy rd
    {SINGLE_INPUT, SPY_RD_TOKEN, "SpyRd", set_cfg_generic_token},

    // Sharp TX
    {SINGLE_INPUT, SHARP_TX_TOKEN, "SharpTX", set_cfg_generic_token},

    // HBD-MDS
    {SINGLE_INPUT, HBD_MDS_TOKEN, "HBDMDS", set_cfg_generic_token},

    // Termination
    {SINGLE_INPUT, NULL, NULL, NULL}};

/**********************************
 * Constructor
 **********************************/
EbConfig *svt_config_ctor() {
    EbConfig *app_cfg = (EbConfig *)calloc(1, sizeof(EbConfig));
    if (!app_cfg)
        return NULL;
    app_cfg->error_log_file      = stderr;
    app_cfg->buffered_input      = -1;
    app_cfg->progress            = 1;
    app_cfg->injector_frame_rate = 60;
    app_cfg->roi_map_file        = NULL;
#ifdef LIBDOVI_FOUND
    app_cfg->dovi_rpus           = NULL;
#endif
#ifdef LIBHDR10PLUS_RS_FOUND
    app_cfg->hdr10plus_json      = NULL;
#endif
    app_cfg->fgs_table_path      = NULL;

    return app_cfg;
}

/**********************************
 * Destructor
 **********************************/
void svt_config_dtor(EbConfig *app_cfg) {
    if (!app_cfg)
        return;
    // Close any files that are open
    if (app_cfg->input_file) {
        if (!app_cfg->input_file_is_fifo)
            fclose(app_cfg->input_file);
        app_cfg->input_file = (FILE *)NULL;
    }

    if (app_cfg->bitstream_file) {
        if (!fseek(app_cfg->bitstream_file, 0, SEEK_SET))
            write_ivf_stream_header(app_cfg, app_cfg->frames_encoded);
        fclose(app_cfg->bitstream_file);
        app_cfg->bitstream_file = (FILE *)NULL;
    }

    if (app_cfg->recon_file) {
        fclose(app_cfg->recon_file);
        app_cfg->recon_file = (FILE *)NULL;
    }

    if (app_cfg->error_log_file && app_cfg->error_log_file != stderr) {
        fclose(app_cfg->error_log_file);
        app_cfg->error_log_file = (FILE *)NULL;
    }

    if (app_cfg->qp_file) {
        fclose(app_cfg->qp_file);
        app_cfg->qp_file = (FILE *)NULL;
    }

    if (app_cfg->stat_file) {
        fclose(app_cfg->stat_file);
        app_cfg->stat_file = (FILE *)NULL;
    }

    if (app_cfg->output_stat_file) {
        fclose(app_cfg->output_stat_file);
        app_cfg->output_stat_file = (FILE *)NULL;
    }

    if (app_cfg->roi_map_file) {
        fclose(app_cfg->roi_map_file);
        app_cfg->roi_map_file = (FILE *)NULL;
    }

#ifdef LIBDOVI_FOUND
    if (app_cfg->dovi_rpus) {
        dovi_rpu_list_free(app_cfg->dovi_rpus);
        app_cfg->dovi_rpus = NULL;
    }
#endif
#ifdef LIBHDR10PLUS_RS_FOUND
    if (app_cfg->hdr10plus_json) {
        hdr10plus_rs_json_free(app_cfg->hdr10plus_json);
        app_cfg->hdr10plus_json = NULL;
    }
#endif

    if (app_cfg->fgs_table_path) {
        free(app_cfg->fgs_table_path);
        app_cfg->fgs_table_path = NULL;
    }

    for (size_t i = 0; i < app_cfg->forced_keyframes.count; ++i) free(app_cfg->forced_keyframes.specifiers[i]);
    free(app_cfg->forced_keyframes.specifiers);
    free(app_cfg->forced_keyframes.frames);

    free((void *)app_cfg->stats);
    free(app_cfg);
    return;
}
EbErrorType enc_channel_ctor(EncChannel *c) {
    c->app_cfg = svt_config_ctor();
    if (!c->app_cfg)
        return EB_ErrorInsufficientResources;

    c->exit_cond        = APP_ExitConditionError;
    c->exit_cond_output = APP_ExitConditionError;
    c->exit_cond_recon  = APP_ExitConditionError;
    c->exit_cond_input  = APP_ExitConditionError;
    c->active           = false;
    return svt_av1_enc_init_handle(&c->app_cfg->svt_encoder_handle, &c->app_cfg->config);
}

void enc_channel_dctor(EncChannel *c, uint32_t inst_cnt) {
    EbConfig *ctx = c->app_cfg;
    if (ctx && ctx->svt_encoder_handle) {
        svt_av1_enc_deinit(ctx->svt_encoder_handle);
        de_init_encoder(ctx, inst_cnt);
    }
    svt_config_dtor(c->app_cfg);
}

/**
 * @brief Find token and its argument
 * @param argc      Argument count
 * @param argv      Argument array
 * @param token     The token to look for
 * @param configStr Output buffer to write the argument to or NULL
 * @return 0 if found, non-zero otherwise
 *
 * @note The configStr buffer must be at least
 *       COMMAND_LINE_MAX_SIZE bytes big.
 *       The argv must contain an additional NULL
 *       element to terminate it, so that
 *       argv[argc] == NULL.
 */
static int32_t find_token(int32_t argc, char *const argv[], char const *token, char *configStr) {
    assert(argv[argc] == NULL);

    if (argc == 0)
        return -1;

    for (int32_t i = argc - 1; i >= 0; i--) {
        if (strcmp(argv[i], token) != 0)
            continue;

        // The argument matches the token.
        // If given, try to copy its argument to configStr
        if (configStr && argv[i + 1] != NULL) {
            strcpy_s(configStr, COMMAND_LINE_MAX_SIZE, argv[i + 1]);
        } else if (configStr) {
            configStr[0] = '\0';
        }

        return 0;
    }
    return -1;
}

/**
 * @brief Finds the config entry for a given config token
 *
 * @param name config token
 * @return ConfigEntry*
 */
static ConfigEntry *find_entry(const char *name) {
    for (size_t i = 0; config_entry[i].name != NULL; ++i) {
        if (!strcmp(config_entry[i].name, name))
            return &config_entry[i];
    }
    return NULL;
}

/**
 * @brief Reads a word from a file, but skips commented lines, also splits on ':'
 *
 * @param fp file to read from
 * @return char* malloc'd word, or NULL if EOF or error
 */
static char *read_word(FILE *fp) {
    char  *word     = NULL;
    size_t word_len = 0;
    int    c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '#') {
            // skip to end of line
            while ((c = fgetc(fp)) != EOF && c != '\n')
                ;
            if (c == '\n')
                continue;
            if (c == EOF)
                break;
        } else if (isspace(c)) {
            // skip whitespace
            continue;
        }
        // read word
        do {
            if (c == ':')
                break;
            char *temp = (char *)realloc(word, ++word_len + 1);
            if (!temp) {
                free(word);
                return NULL;
            }
            word               = temp;
            word[word_len - 1] = c;
            word[word_len]     = '\0';
        } while ((c = fgetc(fp)) != EOF && !isspace(c));
        if (c == EOF || word)
            break;
    }
    return word;
}

static EbErrorType set_config_value(EbConfig *app_cfg, const char *word, const char *value, unsigned instance_idx) {
    const ConfigEntry *entry = find_entry(word);
    if (!entry) {
        fprintf(stderr, "Error channel %u: Config File contains unknown token %s\n", instance_idx + 1, word);
        return EB_ErrorBadParameter;
    }
    const EbErrorType err = entry->scf(app_cfg, entry->token, value);
    if (err != EB_ErrorNone) {
        fprintf(stderr,
                "Error channel %u: Config File contains invalid value %s for token %s\n",
                instance_idx + 1,
                value,
                word);
        return EB_ErrorBadParameter;
    }
    return EB_ErrorNone;
}

/**********************************
* Read Config File
**********************************/
static EbErrorType read_config_file(EbConfig *app_cfg, const char *config_path, uint32_t instance_idx) {
    FILE *config_file;

    // Open the config file
    FOPEN(config_file, config_path, "rb");
    if (!config_file) {
        fprintf(stderr, "Error channel %u: Couldn't open Config File: %s\n", instance_idx + 1, config_path);
        return EB_ErrorBadParameter;
    }

    EbErrorType return_error = EB_ErrorNone;
    char       *word         = NULL;
    char       *value        = NULL;
    while (return_error == EB_ErrorNone && (word = read_word(config_file))) {
        value = read_word(config_file);
        if (value && !strcmp(value, ":")) {
            free(value);
            value = read_word(config_file);
        }
        if (!value) {
            fprintf(stderr,
                    "Error channel %u: Config File: %s is missing a value for %s\n",
                    instance_idx + 1,
                    config_path,
                    word);
            return_error = EB_ErrorBadParameter;
            break;
        }
        return_error = set_config_value(app_cfg, word, value, instance_idx);
    }
    free(word);
    free(value);
    fclose(config_file);
    return return_error;
}

/* get config->rc_stats_buffer from config->input_stat_file */
bool load_twopass_stats_in(EbConfig *cfg) {
    EbSvtAv1EncConfiguration *config = &cfg->config;
#ifdef _WIN32
    int          fd = _fileno(cfg->input_stat_file);
    struct _stat file_stat;
    int          ret = _fstat(fd, &file_stat);
#else
    int         fd = fileno(cfg->input_stat_file);
    struct stat file_stat;
    int         ret = fstat(fd, &file_stat);
#endif
    if (ret) {
        return false;
    }
    config->rc_stats_buffer.buf = malloc(file_stat.st_size);
    if (config->rc_stats_buffer.buf) {
        config->rc_stats_buffer.sz = (uint64_t)file_stat.st_size;
        if (fread(config->rc_stats_buffer.buf, 1, file_stat.st_size, cfg->input_stat_file) !=
            (size_t)file_stat.st_size) {
            return false;
        }
        if (file_stat.st_size == 0) {
            return false;
        }
    }
    return config->rc_stats_buffer.buf != NULL;
}
EbErrorType handle_stats_file(EbConfig *app_cfg, EncPass enc_pass, const SvtAv1FixedBuf *rc_stats_buffer,
                              uint32_t channel_number) {
    switch (enc_pass) {
    case ENC_SINGLE_PASS: {
        const char *stats = app_cfg->stats ? app_cfg->stats : "svtav1_2pass.log";
        if (app_cfg->config.pass == 1) {
            if (!fopen_and_lock(&app_cfg->output_stat_file, stats, true)) {
                fprintf(app_cfg->error_log_file,
                        "Error instance %u: can't open stats file %s for write \n",
                        channel_number + 1,
                        stats);
                return EB_ErrorBadParameter;
            }
        }
        // Final pass
        else if (app_cfg->config.pass == 2) {
            if (!fopen_and_lock(&app_cfg->input_stat_file, stats, false)) {
                fprintf(app_cfg->error_log_file,
                        "Error instance %u: can't read stats file %s for read\n",
                        channel_number + 1,
                        stats);
                return EB_ErrorBadParameter;
            }
            if (!load_twopass_stats_in(app_cfg)) {
                fprintf(app_cfg->error_log_file, "Error instance %u: can't load file %s\n", channel_number + 1, stats);
                return EB_ErrorBadParameter;
            }
        }
        break;
    }

    case ENC_FIRST_PASS: {
        // for combined two passes,
        // we only ouptut first pass stats when user explicitly set the --stats
        if (app_cfg->stats) {
            if (!fopen_and_lock(&app_cfg->output_stat_file, app_cfg->stats, true)) {
                fprintf(app_cfg->error_log_file,
                        "Error instance %u: can't open stats file %s for write \n",
                        channel_number + 1,
                        app_cfg->stats);
                return EB_ErrorBadParameter;
            }
        }
        break;
    }
    case ENC_SECOND_PASS: {
        if (!rc_stats_buffer->sz) {
            fprintf(app_cfg->error_log_file,
                    "Error instance %u: combined multi passes need stats in for the final pass \n",
                    channel_number + 1);
            return EB_ErrorBadParameter;
        }
        app_cfg->config.rc_stats_buffer = *rc_stats_buffer;
        break;
    }

    default: {
        assert(0);
        break;
    }
    }
    return EB_ErrorNone;
}
/******************************************
* Verify Settings
******************************************/
static EbErrorType app_verify_config(EbConfig *app_cfg, uint32_t channel_number) {
    EbErrorType return_error = EB_ErrorNone;

    // Check Input File
    if (app_cfg->input_file == (FILE *)NULL) {
        fprintf(app_cfg->error_log_file, "Error instance %u: Invalid Input File\n", channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->frames_to_be_encoded <= -1) {
        fprintf(app_cfg->error_log_file,
                "Error instance %u: FrameToBeEncoded must be greater than 0\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->buffered_input == 0) {
        fprintf(app_cfg->error_log_file, "Error instance %u: Buffered Input cannot be 0\n", channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->buffered_input < -1) {
        fprintf(app_cfg->error_log_file,
                "Error instance %u: Invalid buffered_input. buffered_input must be -1 or greater "
                "than or equal to 1\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->buffered_input != -1 && app_cfg->y4m_input) {
        fprintf(app_cfg->error_log_file,
                "Error instance %u: Buffered input is currently not available with y4m inputs\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->buffered_input > app_cfg->frames_to_be_encoded) {
        fprintf(app_cfg->error_log_file,
                "Error instance %u: Invalid buffered_input. buffered_input must be less or equal "
                "to the number of frames to be encoded\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->config.use_qp_file == true && app_cfg->qp_file == NULL) {
        fprintf(app_cfg->error_log_file,
                "Error instance %u: Could not find QP file, UseQpFile is set to 1\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->injector > 1) {
        fprintf(app_cfg->error_log_file, "Error Instance %u: Invalid injector [0 - 1]\n", channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (app_cfg->injector_frame_rate > 240 && app_cfg->injector) {
        fprintf(app_cfg->error_log_file,
                "Error Instance %u: The maximum allowed injector_frame_rate is 240 fps\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }
    // Check that the injector frame_rate is non-zero
    if (!app_cfg->injector_frame_rate && app_cfg->injector) {
        fprintf(app_cfg->error_log_file,
                "Error Instance %u: The injector frame rate should be greater than 0 fps \n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (app_cfg->config.frame_rate_numerator == 0 || app_cfg->config.frame_rate_denominator == 0) {
        fprintf(app_cfg->error_log_file,
                "Error Instance %u: The frame_rate_numerator and frame_rate_denominator should be "
                "greater than 0\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    } else if (app_cfg->config.frame_rate_numerator / app_cfg->config.frame_rate_denominator > 240) {
        fprintf(app_cfg->error_log_file,
                "Error Instance %u: The maximum allowed frame_rate is 240 fps\n",
                channel_number + 1);
        return_error = EB_ErrorBadParameter;
    }

    return return_error;
}

static const char *TOKEN_READ_MARKER  = "THIS_TOKEN_HAS_BEEN_READ";
static const char *TOKEN_ERROR_MARKER = "THIS_TOKEN_HAS_ERROR";

/**
 * @brief Finds the arguments for a specific token
 *
 * @param nch number of channels (number of arguemnts to find for a token)
 * @param argc argc from main()
 * @param argv argv from main()
 * @param token token to find
 * @param configStr array of pointers to store the arguments into
 * @param cmd_copy array of tokens based on splitting argv
 * @param arg_copy array of arguments based on splitting argv
 * @return true token was found and configStr was populated
 * @return false token was not found and configStr was not populated
 */
static bool find_token_multiple_inputs(unsigned nch, int argc, char *const argv[], const char *token,
                                       char *configStr[MAX_CHANNEL_NUMBER], const char *cmd_copy[MAX_NUM_TOKENS],
                                       const char *arg_copy[MAX_NUM_TOKENS]) {
    bool return_error   = false;
    bool has_duplicates = false;
    // Loop over all the arguments
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], token))
            continue;
        if (return_error)
            has_duplicates = true;
        return_error = true;
        if (i + 1 >= argc) {
            // if the token is at the end of the command line without arguments
            // set sentinel value
            strcpy_s(configStr[0], COMMAND_LINE_MAX_SIZE, " ");
            return return_error;
        }
        cmd_copy[i] = TOKEN_READ_MARKER; // mark token as read
        // consume arguments
        for (unsigned count = 0; count < nch; ++count) {
            const int j = i + 1 + count;
            if (j >= argc || cmd_copy[j]) {
                // stop if we ran out of arguments or if we hit a token
                strcpy_s(configStr[count], COMMAND_LINE_MAX_SIZE, " ");
                continue;
            }
            strcpy_s(configStr[count], COMMAND_LINE_MAX_SIZE, argv[j]);
            arg_copy[j] = TOKEN_READ_MARKER;
        }
    }

    if (return_error && !strcmp(configStr[0], " ")) {
        // if no argument was found, print an error message
        // we don't support flip switches, so this will need to be changed if we ever do.
        fprintf(stderr, "[SVT-Error]: No argument found for token `%s`\n", token);
        strcpy_s(configStr[0], COMMAND_LINE_MAX_SIZE, TOKEN_ERROR_MARKER);
    }

    if (has_duplicates) {
        fprintf(stderr, "\n[SVT-Warning]: Duplicate option %s specified, only `%s", token, token);
        for (unsigned count = 0; count < nch; ++count) fprintf(stderr, " %s", configStr[count]);
        fprintf(stderr, "` will apply\n\n");
    }

    return return_error;
}

static int check_long(ConfigEntry cfg_entry, ConfigEntry cfg_entry_next) {
    return cfg_entry_next.name ? !strcmp(cfg_entry.name, cfg_entry_next.name) : 0;
}

int get_version(int argc, char *argv[]) {
#ifdef NDEBUG
    static int debug_build = 1;
#else
    static int debug_build = 0;
#endif
    if (find_token(argc, argv, VERSION_TOKEN, NULL))
        return 0;
    printf("SVT-AV1-PSY %s (%s)\n", svt_av1_get_version(), debug_build ? "release" : "debug");
#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf("PSY Release: %s\n", svt_psy_get_version());
#else
    if (strcmp(svt_psy_get_version(), "N/A")) {
        printf("PSY Release: \x1b[32m%s\x1b[0m\n", svt_psy_get_version());
    } else {
        printf("PSY Release: \x1b[38;5;248m%s\x1b[0m\n", svt_psy_get_version());
    }
#endif
    return 1;
}

uint32_t get_help(int32_t argc, char *const argv[]) {
    char config_string[COMMAND_LINE_MAX_SIZE];
    if (find_token(argc, argv, HELP_TOKEN, config_string))
        return 0;

    printf(
        "\x1b[1;4mUsage\x1b[0m: SvtAv1EncApp <options> <-b dst_filename> -i src_filename\n"
        "\n"
        "\x1b[1;4mExamples\x1b[0m:\n"
        "Multi-pass encode (VBR):\n"
        "    SvtAv1EncApp <--stats svtav1_2pass.log> --passes 2 --rc 1 --tbr 1000 -b dst_filename "
        "-i src_filename\n"
        "Multi-pass encode (CRF):\n"
        "    SvtAv1EncApp <--stats svtav1_2pass.log> --passes 2 --rc 0 --crf 43 -b dst_filename -i "
        "src_filename\n"
        "Single-pass encode (VBR):\n"
        "    SvtAv1EncApp --passes 1 --rc 1 --tbr 1000 -b dst_filename -i src_filename\n"
        "\n"
        "\x1b[1;4mOptions\x1b[0m:\n");
    for (ConfigEntry *options_token_index = config_entry_options; options_token_index->token; ++options_token_index) {
        // this only works if short and long token are one after another
        switch (check_long(*options_token_index, options_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n",
                   options_token_index->token,
                   options_token_index[1].token,
                   options_token_index->name);
            ++options_token_index;
            break;
        default:
            printf(options_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   options_token_index->token,
                   options_token_index->name);
        }
    }
    printf("\n\x1b[1;4mEncoder Global Options\x1b[0m:\n");
    for (ConfigEntry *global_options_token_index = config_entry_global_options; global_options_token_index->token;
         ++global_options_token_index) {
        switch (check_long(*global_options_token_index, global_options_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n",
                   global_options_token_index->token,
                   global_options_token_index[1].token,
                   global_options_token_index->name);
            ++global_options_token_index;
            break;
        default:
            printf(global_options_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   global_options_token_index->token,
                   global_options_token_index->name);
        }
    }
    printf("\n\x1b[1;4mRate Control Options\x1b[0m:\n");
    for (ConfigEntry *rc_token_index = config_entry_rc; rc_token_index->token; ++rc_token_index) {
        switch (check_long(*rc_token_index, rc_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n", rc_token_index->token, rc_token_index[1].token, rc_token_index->name);
            ++rc_token_index;
            break;
        default:
            printf(rc_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   rc_token_index->token,
                   rc_token_index->name);
        }
    }
    printf("\n\x1b[1;4mMulti-pass Options\x1b[0m:\n");
    for (ConfigEntry *two_p_token_index = config_entry_2p; two_p_token_index->token; ++two_p_token_index) {
        switch (check_long(*two_p_token_index, two_p_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n",
                   two_p_token_index->token,
                   two_p_token_index[1].token,
                   two_p_token_index->name);
            ++two_p_token_index;
            break;
        default:
            printf(two_p_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   two_p_token_index->token,
                   two_p_token_index->name);
        }
    }
    printf("\n\x1b[1;4mGOP Size & Type Options\x1b[0m:\n");
    for (ConfigEntry *kf_token_index = config_entry_intra_refresh; kf_token_index->token; ++kf_token_index) {
        switch (check_long(*kf_token_index, kf_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n", kf_token_index->token, kf_token_index[1].token, kf_token_index->name);
            ++kf_token_index;
            break;
        default:
            printf(kf_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   kf_token_index->token,
                   kf_token_index->name);
        }
    }
    printf("\n\x1b[1;4mAV1 Specific Options\x1b[0m:\n");
    for (ConfigEntry *sp_token_index = config_entry_specific; sp_token_index->token; ++sp_token_index) {
        switch (check_long(*sp_token_index, sp_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n", sp_token_index->token, sp_token_index[1].token, sp_token_index->name);
            ++sp_token_index;
            break;
        default:
            printf(sp_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   sp_token_index->token,
                   sp_token_index->name);
        }
    }
    printf("\n\x1b[1;4mColor Description Options\x1b[0m:\n");
    for (ConfigEntry *cd_token_index = config_entry_color_description; cd_token_index->token; ++cd_token_index) {
        switch (check_long(*cd_token_index, cd_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n", cd_token_index->token, cd_token_index[1].token, cd_token_index->name);
            ++cd_token_index;
            break;
        default:
            printf(cd_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   cd_token_index->token,
                   cd_token_index->name);
        }
    }

    printf("\n\x1b[1;4mPsychovisual Options\x1b[0m:\n");
    for (ConfigEntry *cd_token_index = config_entry_variance_boost; cd_token_index->token; ++cd_token_index) {
        switch (check_long(*cd_token_index, cd_token_index[1])) {
        case 1:
            printf("  %s, %-25s    %-25s\n", cd_token_index->token, cd_token_index[1].token, cd_token_index->name);
            ++cd_token_index;
            break;
        default:
            printf(cd_token_index->token[1] == '-' ? "      %-25s    %-25s\n" : "      -%-25s   %-25s\n",
                   cd_token_index->token,
                   cd_token_index->name);
        }
    }

    return 1;
}

uint32_t get_color_help(int32_t argc, char *const argv[]) {
    char config_string[COMMAND_LINE_MAX_SIZE];
    if (find_token(argc, argv, COLORH_TOKEN, config_string)) {
        return 0;
    }

    printf("This command line flag reproduces information provided by Appendix A.2 of the SVT-AV1 User Guide.\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The available options for --color-primaries are:\n\n"
#else
    printf(
        "The available options for \x1b[32m--color-primaries\x1b[0m are:\n\n"
#endif
        "\t1: bt709, BT.709\n"
        "\t2: unspecified, default\n"
        "\t4: bt470m, BT.470 System M (historical)\n"
        "\t5: bt470bg, BT.470 System B, G (historical)\n"
        "\t6: bt601, BT.601\n"
        "\t7: smpte240, SMPTE 240\n"
        "\t8: film, Generic film (color filters using illuminant C)\n"
        "\t9: bt2020, BT.2020, BT.2100\n"
        "\t10: xyz, SMPTE 428 (CIE 1921 XYZ)\n"
        "\t11: smpte431, SMPTE RP 431-2\n"
        "\t12: smpte432, SMPTE EG 432-1\n"
        "\t22: ebu3213, EBU Tech. 3213-E\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The available options for --transfer-characteristics are:\n\n"
#else
    printf(
        "The available options for \x1b[32m--transfer-characteristics\x1b[0m are:\n\n"
#endif
        "\t1: bt709, BT.709\n"
        "\t2: unspecified, default\n"
        "\t4: bt470m, BT.470 System M (historical)\n"
        "\t5: bt470bg, BT.470 System B, G (historical)\n"
        "\t6: bt601, BT.601\n"
        "\t7: smpte240, SMPTE 240 M\n"
        "\t8: linear, Linear\n"
        "\t9: log100, Logarithmic (100 : 1 range)\n"
        "\t10: log100-sqrt10, Logarithmic (100 * Sqrt(10) : 1 range)\n"
        "\t11: iec61966, IEC 61966-2-4\n"
        "\t12: bt1361, BT.1361\n"
        "\t13: srgb, sRGB or sYCC\n"
        "\t14: bt2020-10, BT.2020 10-bit systems\n"
        "\t15: bt2020-12, BT.2020 12-bit systems\n"
        "\t16: smpte2084, SMPTE ST 2084, ITU BT.2100 PQ\n"
        "\t17: smpte428, SMPTE ST 428\n"
        "\t18: hlg, BT.2100 HLG, ARIB STD-B67\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The available options for --matrix-coefficients are:\n\n"
#else
    printf(
        "The available options for \x1b[32m--matrix-coefficients\x1b[0m are:\n\n"
#endif
        "\t0: identity, Identity matrix\n"
        "\t1: bt709, BT.709\n"
        "\t2: unspecified, default\n"
        "\t4: fcc, US FCC 73.628\n"
        "\t5: bt470bg, BT.470 System B, G (historical)\n"
        "\t6: bt601, BT.601\n"
        "\t7: smpte240, SMPTE 240 M\n"
        "\t8: ycgco, YCgCo\n"
        "\t9: bt2020-ncl, BT.2020 non-constant luminance, BT.2100 YCbCr\n"
        "\t10: bt2020-cl, BT.2020 constant luminance\n"
        "\t11: smpte2085, SMPTE ST 2085 YDzDx\n"
        "\t12: chroma-ncl, Chromaticity-derived non-constant luminance\n"
        "\t13: chroma-cl, Chromaticity-derived constant luminance\n"
        "\t14: ictcp, BT.2100 ICtCp\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The available options for --color-range are:\n\n"
#else
    printf(
        "The available options for \x1b[32m--color-range\x1b[0m are:\n\n"
#endif
        "\t0: studio (default)\n"
        "\t1: full\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The available options for --chroma-sample-position are:\n\n"
#else
    printf(
        "The available options for \x1b[32m--chroma-sample-position\x1b[0m are:\n\n"
#endif
        "\t0: unknown, default\n"
        "\t1: vertical/left, horizontally co-located with luma samples, vertical position in the middle between "
        "two luma samples\n"
        "\t2: colocated/topleft, co-located with luma samples\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The --mastering-display and --content-light parameters are used to set the mastering display and "
        "content light level in the AV1 bitstream.\n\n");
#else
    printf(
        "The \x1b[32m--mastering-display\x1b[0m and \x1b[32m--content-light\x1b[0m parameters are used to set the "
        "mastering display and content light level in the AV1 bitstream.\n\n");
#endif

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "--mastering-display takes the format of G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) where\n\n"
#else
    printf(
        "\x1b[32m--mastering-display\x1b[0m takes the format of G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) where\n\n"
#endif
        "\t- G(x,y) is the green channel of the mastering display\n"
        "\t- B(x,y) is the blue channel of the mastering display\n"
        "\t- R(x,y) is the red channel of the mastering display\n"
        "\t- WP(x,y) is the white point of the mastering display\n"
        "\t- L(max,min) is the light level of the mastering display\n\n");

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "The x & y values can be coordinates from 0.0 to 1.0, as specified in CIE 1931 while the min,max values "
        "can be floating point values representing candelas per square meter, or nits.\n"
        "The max,min values are generally specified in the range of 0.0 to 1.0 but there are no constraints on "
        "the provided values.\n"
        "Invalid values will be clipped accordingly.\n\n");
#else
    printf(
        "\x1b[38;5;248mThe x & y values can be coordinates from 0.0 to 1.0, as specified in CIE 1931 while the min,max "
        "values can be floating point values representing candelas per square meter, or nits.\n"
        "The max,min values are generally specified in the range of 0.0 to 1.0 but there are no constraints on the "
        "provided values.\n"
        "Invalid values will be clipped accordingly.\x1b[0m\n\n");
#endif

#if defined(_WIN64) || defined(_MSC_VER) || defined(_WIN32)
    printf(
        "--content-light takes the format of max_cll,max_fall where both values are integers clipped into a "
        "range of 0 to 65535.\n");
#else
    printf(
        "\x1b[32m--content-light\x1b[0m takes the format of max_cll,max_fall where both values are integers clipped "
        "into a range of 0 to 65535.\n");
#endif

    return 1;
}

/******************************************************
* Get the number of channels and validate it with input
******************************************************/
uint32_t get_number_of_channels(int32_t argc, char *const argv[]) {
    char config_string[COMMAND_LINE_MAX_SIZE];
    if (find_token(argc, argv, CHANNEL_NUMBER_TOKEN, config_string) == 0) {
        // Set the input file
        uint32_t channel_number = strtol(config_string, NULL, 0);
        if ((channel_number > MAX_CHANNEL_NUMBER) || channel_number == 0) {
            fprintf(
                stderr, "[SVT-Error]: The number of channels has to be within the range [1,%u]\n", MAX_CHANNEL_NUMBER);
            return 0;
        }
        return channel_number;
    }
    return 1;
}

static bool check_two_pass_conflicts(int32_t argc, char *const argv[]) {
    char        config_string[COMMAND_LINE_MAX_SIZE];
    const char *conflicts[] = {
        PASS_TOKEN,
        NULL,
    };
    int         i = 0;
    const char *token;
    while ((token = conflicts[i])) {
        if (find_token(argc, argv, token, config_string) == 0) {
            fprintf(stderr, "[SVT-Error]: --passes is not accepted in combination with %s\n", token);
            return true;
        }
        i++;
    }
    return false;
}
/*
* Returns the number of passes, multi_pass_mode
*/
uint32_t get_passes(int32_t argc, char *const argv[], EncPass enc_pass[MAX_ENC_PASS]) {
    char           config_string[COMMAND_LINE_MAX_SIZE];
    MultiPassModes multi_pass_mode;

    int rc_mode = 0;
    // copied from str_to_rc_mode()
    const struct {
        const char *name;
        uint32_t    mode;
    } rc[] = {
        {"0", 0},
        {"1", 1},
        {"2", 2},
        {"cqp", 0},
        {"crf", 0},
        {"vbr", 1},
        {"cbr", 2},
    };
    const size_t rc_size  = sizeof(rc) / sizeof(rc[0]);
    int          enc_mode = 0;
    // Read required inputs to decide on the number of passes and check the validity of their ranges
    if (find_token(argc, argv, RATE_CONTROL_ENABLE_TOKEN, config_string) == 0) {
        for (size_t i = 0; i < rc_size; i++) {
            if (!strcmp(config_string, rc[i].name)) {
                rc_mode = rc[i].mode;
                break;
            }
        }
        if (rc_mode > 2 || rc_mode < 0) {
            fprintf(stderr, "Error: The rate control mode must be [0 - 2] \n");
            return 0;
        }
    }

    int32_t passes     = -1;
    int     using_fifo = 0;

    if (find_token(argc, argv, INPUT_FILE_LONG_TOKEN, config_string) == 0 ||
        find_token(argc, argv, INPUT_FILE_TOKEN, config_string) == 0) {
        if (!strcmp(config_string, "stdin")) {
            using_fifo = 1;
        } else {
#ifdef _WIN32
            HANDLE in_file = CreateFile(config_string, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (in_file != INVALID_HANDLE_VALUE) {
                using_fifo = GetFileType(in_file) == FILE_TYPE_PIPE;
                CloseHandle(in_file);
            }
#else
            struct stat st;
            using_fifo = !stat(config_string, &st) && S_ISFIFO(st.st_mode);
#endif
        }
    }
    if (find_token(argc, argv, PRESET_TOKEN, config_string) == 0) {
        enc_mode = strtol(config_string, NULL, 0);
        if (enc_mode > MAX_ENC_PRESET || enc_mode < -3) {
            fprintf(stderr, "Error: EncoderMode must be in the range of [-3-%d]\n", MAX_ENC_PRESET);
            return 0;
        }
    }

    if (!find_token(argc, argv, INTRA_PERIOD_TOKEN, NULL) && !find_token(argc, argv, KEYINT_TOKEN, NULL)) {
        fprintf(stderr,
                "[SVT-Warning]: --keyint and --intra-period specified, --keyint will take "
                "precedence!\n");
    }

    if (find_token(argc, argv, INTRA_PERIOD_TOKEN, config_string) == 0 ||
        find_token(argc, argv, KEYINT_TOKEN, config_string) == 0) {
        const bool               is_keyint  = find_token(argc, argv, KEYINT_TOKEN, NULL) == 0;
        const int                max_keyint = 2 * ((1 << 30) - 1);
        int                      ip         = -1;
        EbSvtAv1EncConfiguration c;
        c.multiply_keyint = false;

        svt_av1_enc_parse_parameter(&c, is_keyint ? "keyint" : "intra-period", config_string);
        // temporarily set intraperiod to the max if we are using seconds based keyint
        // we don't know the fps at this point, so we can't get the actual keyint at this point
        ip = c.multiply_keyint && c.intra_period_length > 0 ? max_keyint : c.intra_period_length;
        if (!is_keyint)
            fputs("[SVT-Warning]: --intra-period is deprecated for --keyint\n", stderr);
        if ((ip < -2 || ip > max_keyint) && rc_mode == 0) {
            fprintf(stderr, "[SVT-Error]: The intra period must be [-2, 2^31-2], input %d\n", ip);
            return 0;
        }
        if ((ip < 0) && rc_mode == 1) {
            fprintf(stderr, "[SVT-Error]: The intra period must be > 0 for RateControlMode %d \n", rc_mode);
            return 0;
        }
    }

    if (find_token(argc, argv, PASSES_TOKEN, config_string) == 0) {
        if (str_to_int(PASSES_TOKEN, config_string, &passes))
            return 0;
        if (passes == 0 || passes > 2) {
            fprintf(stderr,
                    "[SVT-Error]: The number of passes has to be within the range [1,2], 2 being "
                    "multi-pass encoding\n");
            return 0;
        }
    }

    if (passes != -1 && check_two_pass_conflicts(argc, argv))
        return 0;

    // set default passes to 1 if not specified by the user
    passes = (passes == -1) ? 1 : passes;

    if (using_fifo && passes > 1) {
        fprintf(stderr, "[SVT-Warning]: The number of passes has to be 1 when using a fifo, using 1-pass\n");
        multi_pass_mode = SINGLE_PASS;
        passes          = 1;
    }
    // Determine the number of passes in CRF mode
    if (rc_mode == 0) {
        if (passes != 1) {
            fprintf(stderr, "[SVT-Error]: Multipass CRF is not supported.\n\n");
            return 0;
        }
        multi_pass_mode = SINGLE_PASS;
    }
    // Determine the number of passes in rate control mode
    else if (rc_mode == 1) {
        if (passes == 1)
            multi_pass_mode = SINGLE_PASS;
        else if (passes > 1) {
            // M11, M12, and M13 are mapped to M10, so treat M11, M12, and M13 the same as M10
            if (enc_mode > ENC_M9) {
                fprintf(stderr, "[SVT-Error]:  Multipass VBR is not supported for preset %d.\n\n", enc_mode);
                return 0;
            } else {
                passes          = 2;
                multi_pass_mode = TWO_PASS;
            }
        }
    } else {
        if (passes > 1) {
            fprintf(stderr, "[SVT-Error]: Multipass CBR is not supported.\n\n");
            return 0;
        }
        multi_pass_mode = SINGLE_PASS;
    }

    // Set the settings for each pass based on multi_pass_mode
    switch (multi_pass_mode) {
    case SINGLE_PASS: enc_pass[0] = ENC_SINGLE_PASS; break;
    case TWO_PASS:
        enc_pass[0] = ENC_FIRST_PASS;
        enc_pass[1] = ENC_SECOND_PASS;
        break;
    default: break;
    }

    return passes;
}

static bool is_negative_number(const char *string) {
    char *end;
    return strtol(string, &end, 10) < 0 && *end == '\0';
}

// Computes the number of frames in the input file
int32_t compute_frames_to_be_encoded(EbConfig *app_cfg) {
    uint64_t file_size   = 0;
    int32_t  frame_count = 0;
    uint32_t frame_size;

    // Pipes contain data streams whose end we cannot know before we reach it.
    // For pipes, we leave it up to the eof logic to detect how many frames to eventually encode.
    if (app_cfg->input_file == stdin || app_cfg->input_file_is_fifo)
        return -1;

    if (app_cfg->input_file) {
        uint64_t curr_loc = ftello(app_cfg->input_file); // get current fp location
        fseeko(app_cfg->input_file, 0L, SEEK_END);
        file_size = ftello(app_cfg->input_file);
        fseeko(app_cfg->input_file, curr_loc, SEEK_SET); // seek back to that location
    }
    frame_size = app_cfg->input_padded_width * app_cfg->input_padded_height; // Luma
    frame_size += 2 * (frame_size >> (3 - app_cfg->config.encoder_color_format)); // Add Chroma
    frame_size = frame_size << ((app_cfg->config.encoder_bit_depth == 10) ? 1 : 0);

    if (frame_size == 0)
        return -1;

    frame_count = (int32_t)(file_size / frame_size);

    if (frame_count == 0)
        return -1;

    return frame_count;
}

static bool warn_legacy_token(const char *const token) {
    static struct warn_set {
        const char *old_token;
        const char *new_token;
    } warning_set[] = {
        {"-adaptive-quantization", ADAPTIVE_QP_ENABLE_NEW_TOKEN},
        {"-bit-depth", INPUT_DEPTH_TOKEN},
        {"-enc-mode", PRESET_TOKEN},
        {"-intra-period", KEYINT_TOKEN},
        {"-lad", LOOKAHEAD_NEW_TOKEN},
        {"-mfmv", MFMV_ENABLE_NEW_TOKEN},
        {"-qp-file", QP_FILE_NEW_TOKEN},
        {"-stat-report", STAT_REPORT_NEW_TOKEN},
        {NULL, NULL},
    };
    for (struct warn_set *tok = warning_set; tok->old_token; ++tok) {
        if (strcmp(token, tok->old_token))
            continue;
        fprintf(stderr, "[SVT-Error]: %s has been removed, use %s instead\n", tok->old_token, tok->new_token);
        return true;
    }
    return false;
}

static void free_config_strings(unsigned nch, char *config_strings[MAX_CHANNEL_NUMBER]) {
    for (unsigned i = 0; i < nch; ++i) free(config_strings[i]);
}

static EbErrorType read_fgs_table(EbConfig *cfg) {
    EbErrorType   ret = EB_ErrorBadParameter;
    AomFilmGrain *film_grain;
    FILE         *file;
    FOPEN(file, cfg->fgs_table_path, "r");

    if (!file)
        return EB_ErrorBadParameter;

    // Read in one extra character as there should be a newline
    char magic[9];
    if (!fread(magic, 9, 1, file) || strncmp(magic, "filmgrn1", 8)) {
        fprintf(stderr, "invalid grain table magic %s\n", cfg->fgs_table_path);
        fclose(file);
        return ret;
    }

    film_grain = (AomFilmGrain *)calloc(1, sizeof(AomFilmGrain));

    while (!feof(file)) {
        int num_read = fscanf(file,
                              "E %*d %*d %d %hu %d\n",
                              &film_grain->apply_grain,
                              &film_grain->random_seed,
                              &film_grain->update_parameters);

        if (num_read == 0 && feof(file)) {
            fprintf(stderr, "invalid grain table %s\n", cfg->fgs_table_path);
            goto fail;
        }
        if (num_read != 3) {
            fprintf(stderr, "Unable to read entry header. Read %d != 3\n", num_read);
            goto fail;
        }

        if (film_grain->update_parameters) {
            num_read = fscanf(file,
                              "p %d %d %d %d %d %d %d %d %d %d %d %d\n",
                              &film_grain->ar_coeff_lag,
                              &film_grain->ar_coeff_shift,
                              &film_grain->grain_scale_shift,
                              &film_grain->scaling_shift,
                              &film_grain->chroma_scaling_from_luma,
                              &film_grain->overlap_flag,
                              &film_grain->cb_mult,
                              &film_grain->cb_luma_mult,
                              &film_grain->cb_offset,
                              &film_grain->cr_mult,
                              &film_grain->cr_luma_mult,
                              &film_grain->cr_offset);
            if (num_read != 12) {
                fprintf(stderr, "Unable to read entry header. Read %d != 12\n", num_read);
                goto fail;
            }
            if (!fscanf(file, "\tsY %d ", &film_grain->num_y_points)) {
                fprintf(stderr, "Unable to read num y points\n");
                goto fail;
            }
            for (int i = 0; i < film_grain->num_y_points; ++i) {
                if (2 !=
                    fscanf(file, "%d %d", &film_grain->scaling_points_y[i][0], &film_grain->scaling_points_y[i][1])) {
                    fprintf(stderr, "Unable to read y scaling points\n");
                    goto fail;
                }
            }
            if (!fscanf(file, "\n\tsCb %d", &film_grain->num_cb_points)) {
                fprintf(stderr, "Unable to read num cb points\n");
                goto fail;
            }
            for (int i = 0; i < film_grain->num_cb_points; ++i) {
                if (2 !=
                    fscanf(file, "%d %d", &film_grain->scaling_points_cb[i][0], &film_grain->scaling_points_cb[i][1])) {
                    fprintf(stderr, "Unable to read cb scaling points\n");
                    goto fail;
                }
            }
            if (!fscanf(file, "\n\tsCr %d", &film_grain->num_cr_points)) {
                fprintf(stderr, "Unable to read num cr points\n");
                goto fail;
            }
            for (int i = 0; i < film_grain->num_cr_points; ++i) {
                if (2 !=
                    fscanf(file, "%d %d", &film_grain->scaling_points_cr[i][0], &film_grain->scaling_points_cr[i][1])) {
                    fprintf(stderr, "Unable to read cr scaling points\n");
                    goto fail;
                }
            }

            if (fscanf(file, "\n\tcY")) {
                fprintf(stderr, "Unable to read Y coeffs header (cY)\n");
                goto fail;
            }
            const int n = 2 * film_grain->ar_coeff_lag * (film_grain->ar_coeff_lag + 1);
            for (int i = 0; i < n; ++i) {
                if (1 != fscanf(file, "%d", &film_grain->ar_coeffs_y[i])) {
                    fprintf(stderr, "Unable to read Y coeffs\n");
                    goto fail;
                }
            }
            if (fscanf(file, "\n\tcCb")) {
                fprintf(stderr, "Unable to read Cb coeffs header (cCb)\n");
                goto fail;
            }
            for (int i = 0; i <= n; ++i) {
                if (1 != fscanf(file, "%d", &film_grain->ar_coeffs_cb[i])) {
                    fprintf(stderr, "Unable to read Cb coeffs\n");
                    goto fail;
                }
            }
            if (fscanf(file, "\n\tcCr")) {
                fprintf(stderr, "Unable read to Cr coeffs header (cCr)\n");
                goto fail;
            }
            for (int i = 0; i <= n; ++i) {
                if (1 != fscanf(file, "%d", &film_grain->ar_coeffs_cr[i])) {
                    fprintf(stderr, "Unable to read Cr coeffs\n");
                    goto fail;
                }
            }
            if (fscanf(file, "\n")) {
                // optional newline at end of file,
            }
        }

        // TODO Add functionality to read multiple grain table entries
        break;
    }

    fclose(file);

    film_grain->apply_grain = 1;
    film_grain->ignore_ref  = 1;
    cfg->config.fgs_table   = film_grain;

    return EB_ErrorNone;
fail:
    free(film_grain);

    fclose(file);
    return ret;
}

/******************************************
* Read Command Line
******************************************/
EbErrorType read_command_line(int32_t argc, char *const argv[], EncChannel *channels, uint32_t num_channels) {
    EbErrorType return_error = EB_ErrorNone;
    char        config_string[COMMAND_LINE_MAX_SIZE]; // for one input options
    char       *config_strings[MAX_CHANNEL_NUMBER]; // for multiple input options
    const char *cmd_copy[MAX_NUM_TOKENS]; // keep track of extra tokens
    const char *arg_copy[MAX_NUM_TOKENS]; // keep track of extra arguments
    uint32_t    index = 0;
    int32_t     ret_y4m;

    for (index = 0; index < num_channels; ++index)
        config_strings[index] = (char *)malloc(sizeof(char) * COMMAND_LINE_MAX_SIZE);
    for (int i = 0; i < MAX_NUM_TOKENS; ++i) {
        cmd_copy[i] = NULL;
        arg_copy[i] = NULL;
    }

    // Copy tokens into a temp token buffer hosting all tokens that are passed through the command line
    for (int32_t token_index = 0; token_index < argc; ++token_index) {
        if (!is_negative_number(argv[token_index])) {
            if (argv[token_index][0] == '-' && argv[token_index][1] != '\0')
                cmd_copy[token_index] = argv[token_index];
            else if (token_index)
                arg_copy[token_index] = argv[token_index];
        }
    }

    // First handle --nch and --passes as a single argument options
    find_token_multiple_inputs(1, argc, argv, CHANNEL_NUMBER_TOKEN, config_strings, cmd_copy, arg_copy);
    find_token_multiple_inputs(1, argc, argv, PASSES_TOKEN, config_strings, cmd_copy, arg_copy);

    /***************************************************************************************************/
    /****************  Find configuration files tokens and call respective functions  ******************/
    /***************************************************************************************************/
    // Find the Config File Path in the command line
    if (find_token_multiple_inputs(num_channels, argc, argv, CONFIG_FILE_TOKEN, config_strings, cmd_copy, arg_copy)) {
        // Parse the config file
        for (index = 0; index < num_channels; ++index) {
            EncChannel *c   = channels + index;
            c->return_error = (EbErrorType)read_config_file(c->app_cfg, config_strings[index], index);
            return_error    = (EbErrorType)(return_error & c->return_error);
        }
    } else if (find_token_multiple_inputs(
                   num_channels, argc, argv, CONFIG_FILE_LONG_TOKEN, config_strings, cmd_copy, arg_copy)) {
        // Parse the config file
        for (index = 0; index < num_channels; ++index) {
            EncChannel *c   = channels + index;
            c->return_error = (EbErrorType)read_config_file(c->app_cfg, config_strings[index], index);
            return_error    = (EbErrorType)(return_error & c->return_error);
        }
    } else {
        if (find_token(argc, argv, CONFIG_FILE_TOKEN, config_string) == 0) {
            fprintf(stderr, "Error: Config File Token Not Found\n");
            free_config_strings(num_channels, config_strings);
            return EB_ErrorBadParameter;
        }
        return_error = EB_ErrorNone;
    }

    /********************************************************************************************************/
    /***********   Find SINGLE_INPUT configuration parameter tokens and call respective functions  **********/
    /********************************************************************************************************/

    // Check tokens for invalid tokens
    {
        bool next_is_value = false;
        for (char *const *indx = argv + 1; *indx; ++indx) {
            // stop at --
            if (!strcmp(*indx, "--"))
                break;
            // skip the token if the previous token was an argument
            // assumes all of our tokens flip flop between being an argument and a value
            if (next_is_value) {
                next_is_value = false;
                continue;
            }
            // Check removed tokens
            if (warn_legacy_token(*indx)) {
                free_config_strings(num_channels, config_strings);
                return EB_ErrorBadParameter;
            }
            // exclude single letter tokens
            if ((*indx)[0] == '-' && (*indx)[1] != '-' && (*indx)[2] != '\0') {
                fprintf(stderr, "[SVT-Error]: single dash long tokens have been removed!\n");
                free_config_strings(num_channels, config_strings);
                return EB_ErrorBadParameter;
            }
            next_is_value = true;
        }
    }

    // Parse command line for tokens
    for (ConfigEntry *entry = config_entry; entry->token; ++entry) {
        if (entry->type != SINGLE_INPUT)
            continue;
        if (!find_token_multiple_inputs(num_channels, argc, argv, entry->token, config_strings, cmd_copy, arg_copy))
            continue;
        if (!strcmp(TOKEN_ERROR_MARKER, config_strings[0])) {
            free_config_strings(num_channels, config_strings);
            return EB_ErrorBadParameter;
        }
        // When a token is found mark it as found in the temp token buffer
        // Fill up the values corresponding to each channel
        for (uint32_t chan = 0; chan < num_channels; ++chan) {
            if (!strcmp(config_strings[chan], " "))
                break;
            // Mark the value as found in the temp argument buffer
            EbErrorType err             = (entry->scf)(channels[chan].app_cfg, entry->token, config_strings[chan]);
            channels[chan].return_error = (EbErrorType)(channels[chan].return_error | err);
            return_error                = (EbErrorType)(return_error & channels[chan].return_error);
        }
    }

    /***************************************************************************************************/
    /********************** Parse parameters from input file if in y4m format **************************/
    /********************** overriding config file and command line inputs    **************************/
    /***************************************************************************************************/

    for (index = 0; index < num_channels; ++index) {
        EncChannel *c = channels + index;
        if (c->app_cfg->y4m_input == true) {
            ret_y4m = read_y4m_header(c->app_cfg);
            if (ret_y4m == EB_ErrorBadParameter) {
                fprintf(stderr, "Error found when reading the y4m file parameters.\n");
                free_config_strings(num_channels, config_strings);
                return EB_ErrorBadParameter;
            }
        }
    }

    for (index = 0; index < num_channels; ++index) {
        EncChannel *c   = channels + index;
        EbConfig   *cfg = c->app_cfg;
        if (cfg->fgs_table_path) {
            if (cfg->config.film_grain_denoise_strength > 0) {
                fprintf(stderr,
                        "Warning: Both film-grain-denoise and fgs-table were specified\nfilm-grain-denoise will be "
                        "disabled\n");
                cfg->config.film_grain_denoise_strength = 0;
            }
            c->return_error = read_fgs_table(cfg);
            return_error    = (EbErrorType)(return_error & c->return_error);
        }
    }

    /***************************************************************************************************/
    /**************************************   Verify configuration parameters   ************************/
    /***************************************************************************************************/
    // Verify the config values
    if (return_error == 0) {
        return_error = EB_ErrorBadParameter;
        for (index = 0; index < num_channels; ++index) {
            EncChannel *c = channels + index;
            if (c->return_error == EB_ErrorNone) {
                EbConfig *app_cfg = c->app_cfg;
                c->return_error   = app_verify_config(app_cfg, index);
                // set inj_frame_rate to q16 format
                if (c->return_error == EB_ErrorNone && app_cfg->injector == 1)
                    app_cfg->injector_frame_rate <<= 16;

                // Assuming no errors, add padding to width and height
                if (c->return_error == EB_ErrorNone) {
                    app_cfg->input_padded_width  = app_cfg->config.source_width;
                    app_cfg->input_padded_height = app_cfg->config.source_height;
                }

                const int32_t input_frame_count = compute_frames_to_be_encoded(app_cfg);
                const bool    n_specified       = app_cfg->frames_to_be_encoded != 0;

                // Assuming no errors, set the frames to be encoded to the number of frames in the input yuv
                if (c->return_error == EB_ErrorNone && !n_specified)
                    app_cfg->frames_to_be_encoded = input_frame_count - app_cfg->frames_to_be_skipped;

                // For pipe input it is fine if we have -1 here (we will update on end of stream)
                if (app_cfg->frames_to_be_encoded == -1 && app_cfg->input_file != stdin &&
                    !app_cfg->input_file_is_fifo) {
                    fprintf(app_cfg->error_log_file,
                            "Error instance %u: Input yuv does not contain enough frames \n",
                            index + 1);
                    c->return_error = EB_ErrorBadParameter;
                }
                if (input_frame_count != -1 && app_cfg->frames_to_be_skipped >= input_frame_count) {
                    fprintf(app_cfg->error_log_file,
                            "Error instance %u: FramesToBeSkipped is greater than or equal to the "
                            "number of frames detected\n",
                            index + 1);
                    c->return_error = EB_ErrorBadParameter;
                }

                // Force the injector latency mode, and injector frame rate when speed control is on
                if (c->return_error == EB_ErrorNone && app_cfg->speed_control_flag == 1)
                    app_cfg->injector = 1;
            }
            return_error = (EbErrorType)(return_error & c->return_error);
        }
    }

    bool has_cmd_notread = false;
    for (int i = 0; i < argc; ++i) {
        if (cmd_copy[i] && strcmp(TOKEN_READ_MARKER, cmd_copy[i])) {
            if (!has_cmd_notread)
                fprintf(stderr, "Unprocessed tokens: ");
            fprintf(stderr, "%s ", argv[i]);
            has_cmd_notread = true;
        }
    }
    if (has_cmd_notread) {
        fprintf(stderr, "\n\n");
        return_error = EB_ErrorBadParameter;
    }
    bool has_arg_notread = false;
    bool maybe_token     = false;
    for (int i = 0; i < argc; ++i) {
        if (arg_copy[i] && strcmp(TOKEN_READ_MARKER, arg_copy[i])) {
            if (!has_arg_notread)
                fprintf(stderr, "Unprocessed arguments: ");
            fprintf(stderr, "%s ", argv[i]);
            maybe_token |= !!strchr(arg_copy[i], '-');
            has_arg_notread = true;
        }
    }
    if (maybe_token) {
        fprintf(stderr, "\nMaybe missing spacing between tokens");
    }
    if (has_arg_notread) {
        fprintf(stderr, "\n\n");
        return_error = EB_ErrorBadParameter;
    }

    for (index = 0; index < num_channels; ++index) free(config_strings[index]);

    return return_error;
}
