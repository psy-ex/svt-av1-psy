#!/bin/sh
#
# Copyright(c) 2019 Intel Corporation
#
# This source code is subject to the terms of the BSD 2 Clause License and
# the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
# was not distributed with this source code in the LICENSE file, you can
# obtain it at www.aomedia.org/license/software. If the Alliance for Open
# Media Patent License 1.0 was not distributed with this source code in the
# PATENTS file, you can obtain it at www.aomedia.org/license/patent.
#

# Sets context of if running in the script.
# Helpful when copying and pasting functions and debuging.
if printf '%s' "$0" | grep -q '\.sh'; then
    IN_SCRIPT=true
fi

# Fails the script if any of the commands error (Other than if and some others)
set -e

# Function to print to stderr or stdout while stripping preceding tabs(spaces)
# If tabs are desired, use cat
print_message() {
    if [ "$1" = "stdout" ] || [ "$1" = "-" ]; then
        shift
        printf '%b\n' "${@:-Unknown Message}" | sed 's/^[ \t]*//'
    else
        printf '%b\n' "${@:-Unknown Message}" | sed 's/^[ \t]*//' >&2
    fi
}

# Convenient function to exit with message
die() {
    print_message "${@:-Unknown error}"
    if ${IN_SCRIPT:-false}; then
        print_message "The script will now exit."
        exit 1
    fi
}

# Function for changing directory.
cd_safe() {
    if (cd "$1"); then
        cd "$1"
    else
        _dir="$1"
        shift
        die "${@:-Failed cd to $_dir.}"
    fi
}

# used for resolving certain paths relative to original caller like -t
prev_pwd=
${IN_SCRIPT:-false} && {
    prev_pwd=$PWD
    cd_safe "$(cd "$(dirname "$0")" > /dev/null 2>&1 && pwd -P)"
}

# Help message
echo_help() {
    cat << EOF
Usage: $0 [OPTION] ... -- [OPTIONS FOR CMAKE]
--enable-*/--disable-* options shown are not the default
For each enable-*, there is a disable-* option, and vice versa.

-a, --all, all          Builds release and debug
    --asm, asm=*        Set assembly compiler [$ASM]
-b, --bindir, bindir=*  Directory to install binaries
    --cc, cc=*          Set C compiler [$CC]
    --cxx, cxx=*        Set CXX compiler [$CXX]
    --c-only, c-only    Compile only C code
    --clean, clean      Remove build and Bin folders
    --debug, debug      Build debug
    --disable-avx512,   Disable building avx512 code
    disable-avx512
    --enable-lto,       Enable link time optimization
    enable-lto
    --enable-dovi,   Enable support for Dolby Vision RPUs (if dovi lib is found)
    enable-dovi
    --enable-hdr10plus,   Enable support for HDR10+ metadata (if hdr10plus lib is found)
    enable-hdr10plus
    --disable-native,   Disable the use of -march=native
    disable-native
    --enable-pgo,       Enable profile guided optimization
    enable-pgo
    --shared, shared    Build shared libs
-x, --static, static    Build static libs
    --native, native    Build for native performance (march=native)
-g, --gen, gen=*        Set CMake generator
-i, --install, install  Install build [Default release]
-j, --jobs, jobs=*      Set number of jobs for make/CMake [$jobs]
    --no-apps, no-apps  Don't build the apps, only build the libs
-p, --prefix, prefix=*  Set installation prefix
    --pgo-dir,          Directory to store the pgo profiles
    pgo-dir=*
    --pgo-compile-gen,  Compile PGO up to the generate stage
    pgo-compile-gen
    --pgo-compile-use,  Compile PGO up to the use stage
    pgo-compile-use
    --pgo-videos,       Directory of y4m videos to use for PGO instead
    pgo-videos=         of objective-1-fast
    --release, release  Build release
    --sanitizer,        Build and enable using sanitizer
    sanitizer=*
-s, --target_system,    Set CMake target system
    target_system=*
    --test, test        Build Unit Tests
-t, --toolchain,        Set CMake toolchain file
    toolchain=*
    --android-ndk,      Set path to android NDK for android builds
-v, --verbose, verbose  Print out commands
    --minimal-build,    Enable minimal build
    minimal-build
    --external-cpuinfo,
    external-cpuinfo    Use external cpuinfo library

Example usage:
    build.sh -xi debug test
    build.sh jobs=8 all cc=clang cxx=clang++
    build.sh jobs=8 all cc=clang cxx=clang++ enable-avx512 enable-lto enable-dovi asm=nasm static native verbose
    build.sh -j 4 all -t "https://gist.githubusercontent.com/peterspackman/8cf73f7f12ba270aa8192d6911972fe8/raw/mingw-w64-x86_64.cmake"
    build.sh generator=Xcode cc=clang

Options for specifying android build targets -
    build.sh -s Android -t cmake/toolchains/android_aarch64_toolchain.cmake --android-ndk <Path to NDK>
EOF
}

configure() (
    build_type=Release clean=true
    while [ -n "$*" ]; do
        case $(printf %s "$1" | tr '[:upper:]' '[:lower:]') in
        release) build_type=Release && shift ;;
        debug) build_type=Debug && shift ;;
        no_clean) clean=false && shift ;;
        *) break ;;
        esac
    done
    $clean && rm -rf $build_type
    mkdir -p $build_type > /dev/null 2>&1
    cd_safe $build_type
    cmake ../../.. -DCMAKE_BUILD_TYPE=$build_type $CMAKE_EXTRA_FLAGS "$@"
)

# Usage: build <release|debug> [test]
build() (
    build_type=Release clean=''
    while [ -n "$*" ]; do
        case $(printf %s "$1" | tr '[:upper:]' '[:lower:]') in
        release) build_type=Release && shift ;;
        debug) build_type=Debug && shift ;;
        clean) clean=true && shift ;;
        *) break ;;
        esac
    done
    if cmake --build 2>&1 | grep -q parallel; then
        cmake --build $build_type --config $build_type ${clean:+--clean-first} --parallel "$jobs" "$@"
    elif [ -f "$build_type/Makefile" ]; then
        ${clean:+make -C "$build_type" clean}
        make -C "$build_type" -j "$jobs" "$@"
    else
        cmake --build $build_type --config $build_type ${clean:+--clean-first} "$@"
    fi
)

check_executable() (
    print_exec=false
    while true; do
        case $1 in
        -p) print_exec=true && shift ;;
        *) break ;;
        esac
    done
    [ -n "$1" ] && command_to_check="$1" || return 1
    shift
    if [ -x "$command_to_check" ]; then
        $print_exec && printf '%s\n' "$command_to_check"
        return 0
    fi
    IFS="${IFS}:"
    for d in "$@" $PATH; do
        if [ -x "$d/$command_to_check" ]; then
            $print_exec && printf '%s\n' "$d/$command_to_check"
            return 0
        fi
    done
    return 127
)

install_build() (
    build_type=Release
    sudo=$(check_executable -p sudo) || :
    while [ -n "$*" ]; do
        case $(printf %s "$1" | tr '[:upper:]' '[:lower:]') in
        release) build_type="Release" && shift ;;
        debug) build_type="Debug" && shift ;;
        *) break ;;
        esac
    done

    { [ -d $build_type ] && cd_safe $build_type; } ||
        die "Unable to find the build folder. Did the build command run?"
    cmake --build . --target install --config $build_type || {
        test -n "$sudo" &&
            eval ${sudo:+echo cmake failed to install, trying with sudo && $sudo cmake --build . --target install --config $build_type}
    } || die "Unable to run install"
)

if [ -z "$CC" ] && [ "$(uname -a | cut -c1-5)" != "MINGW" ]; then
    if ! CC=$(check_executable -p icc /opt/intel/bin) && ! CC=$(check_executable -p clang); then
        ! CC=$(check_executable -p gcc) &&
            ! CC=$(check_executable -p cc) &&
            die "No suitable c compiler found in path" \
                "Please either install one or set it via cc=*"
        print_message "Unable to find clang or icc, it is recommended to use clang" "Currently using $CC"
    fi
    export CC
fi

if [ -z "$CXX" ] && [ "$(uname -a | cut -c1-5)" != "MINGW" ]; then
    if ! CXX=$(check_executable -p icpc /opt/intel/bin) && ! CXX=$(check_executable -p clang++); then
        ! CXX=$(check_executable -p g++) &&
            ! CXX=$(check_executable -p c++) &&
            die "No suitable c++ compiler found in path" \
                "Please either install one or set it via cxx=*"
        print_message "Unable to find clang++ or icpc, it is recommended to use clang++" "Currently using $CXX"
    fi
    export CXX
fi

build_release=false
build_debug=false
build_install=false

PGO_COMPILE_STAGE=none

parse_options() {
    while true; do
        [ -z "$1" ] && break
        case $(printf %s "$1" | tr '[:upper:]' '[:lower:]') in
        help) echo_help && ${IN_SCRIPT:-false} && exit ;;
        all) build_debug=true build_release=true && shift ;;
        asm=*)
            check_executable "${1#*=}" &&
                CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_ASM_NASM_COMPILER=$(check_executable -p "${1#*=}")" &&
                case $1 in
                *nasm*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DENABLE_NASM=ON" ;;
                esac
            shift
            ;;
        bindir=*)
            CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_INSTALL_BINDIR=${1#*=}"
            shift
            ;;
        cc=*)
            if check_executable "${1#*=}"; then
                CC=$(check_executable -p "${1#*=}")
                export CC
            fi
            shift
            ;;
        cxx=*)
            if check_executable "${1#*=}"; then
                CXX=$(check_executable -p "${1#*=}")
                export CXX
            fi
            shift
            ;;
        c-only) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCOMPILE_C_ONLY=ON" && shift ;;
        clean)
            for d in *; do
                [ -d "$d" ] && rm -rf "$d"
            done
            for d in ../../Bin/*; do
                [ -d "$d" ] && rm -rf "$d"
            done
            shift && ${IN_SCRIPT:-false} && exit
            ;;
        debug) build_debug=true && shift ;;
        disable*)
            case ${1#disable-} in
            avx512) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DENABLE_AVX512=OFF" ;;
            lto) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_LTO=OFF" ;;
            native) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DNATIVE=OFF" ;;
            pgo) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_PGO=OFF" PGO_COMPILE_STAGE=none ;;
            *) print_message "Unknown option: $1" ;;
            esac
            shift
            ;;
        enable*)
            case ${1#enable-} in
            avx512) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DENABLE_AVX512=ON" ;;
            lto) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_LTO=ON" ;;
            native) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DNATIVE=ON" ;;
            pgo)
                CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_PGO=ON"
                case $PGO_COMPILE_STAGE in
                none) PGO_COMPILE_STAGE=all ;;
                esac
                ;;
            libdovi) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DLIBDOVI_FOUND=1" ;;
            libhdr10plus) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DLIBHDR10PLUS_RS_FOUND=1" ;;
            dovi) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DLIBDOVI_FOUND=1" ;;
            hdr10plus) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DLIBHDR10PLUS_RS_FOUND=1" ;;
            *) print_message "Unknown option: $1" ;;
            esac
            shift
            ;;
        shared) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DBUILD_SHARED_LIBS=ON" && shift ;;
        static) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DBUILD_SHARED_LIBS=OFF" && shift ;;
        native) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DNATIVE=ON" && shift ;;
        gen=*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -G${1#*=}" && shift ;;
        install) build_install=true && shift ;;
        jobs=*) jobs="${1#*=}" && shift ;;
        no-apps) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DBUILD_APPS=OFF" && shift ;;
        prefix=*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_INSTALL_PREFIX=${1#*=}" && shift ;;
        pgo-dir=*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_PGO_DIR=${1#*=}" && shift ;;
        pgo-compile-gen) PGO_COMPILE_STAGE=gen && shift ;;
        pgo-compile-use) PGO_COMPILE_STAGE=use && shift ;;
        pgo-videos=*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSVT_AV1_PGO_CUSTOM_VIDEOS=${1#*=}" && shift ;;
        release) build_release=true && shift ;;
        sanitizer=*) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DSANITIZER=${1#*=}" && shift ;;
        target_system=*)
            CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_SYSTEM_NAME=${1#*=}"
            shift
            ;;
        tests) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DBUILD_TESTING=ON" && shift ;;
        toolchain=*)
            toolchain=''
            url=${1#*=}
            case $(echo "$url" | cut -c1-4) in
            http*)
                toolchain=${url%%\?*}
                toolchain=${toolchain##*/}
                toolchain=$PWD/$toolchain
                curl --connect-timeout 15 --retry 3 --retry-delay 5 -sfLk -o "$toolchain" "$url"
                ;;
            *) toolchain="$(
                case ${url%/*} in
                */*)
                    [ -n "$prev_pwd" ] && cd "$prev_pwd"
                    cd "${url%/*}"
                    ;;
                esac
                pwd -P 2> /dev/null || pwd
            )/${url##*/}" ;;
            esac
            CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_TOOLCHAIN_FILE=$toolchain" && shift
            ;;
        android-ndk=*)
            CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_ANDROID_NDK=${1#*=}"
            shift
            ;;
        verbose) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_VERBOSE_MAKEFILE=1" && shift ;;
        minimal-build) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DMINIMAL_BUILD=ON" && shift ;;
        external-cpuinfo) CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DUSE_EXTERNAL_CPUINFO=ON" && shift ;;
        *) print_message "Unknown option: $1" && shift ;;
        esac
    done
}

parse_equal_option() {
    case $1 in
    *=*) parse_options "$(printf %s "$1" | cut -c3- | cut -d= -f1 | tr '[:upper:]' '[:lower:]')=${1#*=}" ;;
    *) parse_options "$(printf %s "$1" | cut -c3- | cut -d= -f1 | tr '[:upper:]' '[:lower:]')=$2" ;;
    esac
}

parse_options enable-native

if [ -z "$*" ]; then
    build_release=true
else
    while [ -n "$*" ]; do
        case $1 in
        --*) # Handle --* based args
            match=$(printf %s "${1#--}" | cut -d= -f1 | tr '[:upper:]' '[:lower:]')
            case $match in
            # Stop on "--", pass the rest to cmake
            "") shift && break ;;
            help) parse_options help && shift ;;
            all) parse_options debug release && shift ;;
            c-only) parse_options c-only && shift ;;
            clean) parse_options clean && shift ;;
            debug) parse_options debug && shift ;;
            disable* | enable*) parse_options "$match" && shift ;;
            install) parse_options install && shift ;;
            no-apps) parse_options no-apps && shift ;;
            pgo-compile-gen) parse_options pgo-compile-gen && shift ;;
            pgo-compile-use) parse_options pgo-compile-use && shift ;;
            release) parse_options release && shift ;;
            shared) parse_options shared && shift ;;
            static) parse_options static && shift ;;
            native) parse_options native && shift ;;
            toolchain) parse_options toolchain="$2" && shift ;;
            test) parse_options tests && shift ;;
            verbose) parse_options verbose && shift ;;
            minimal-build) parse_options minimal-build && shift ;;
            external-cpuinfo) parse_options external-cpuinfo && shift ;;
            asm | bindir | cc | cxx | gen | jobs | pgo-dir | pgo-videos | prefix | sanitizer | target_system | android-ndk)
                parse_equal_option "$1" "$2"
                case $1 in
                *=*) shift ;;
                *) shift 2 ;;
                esac
                ;;
            *) die "Error, unknown option: $1" ;;
            esac
            ;;
        -*) # Handle -* based args. Currently doesn't differentiate upper and lower since there's not need at the momment.
            i=2
            match=$1
            shift
            while [ $i -ne $((${#match} + 1)) ]; do
                case $(echo "$match" | cut -c$i | tr '[:upper:]' '[:lower:]') in
                h) parse_options help ;;
                a)
                    parse_options all
                    i=$((i + 1))
                    ;;
                b)
                    parse_options bindir="$1"
                    i=$((i + 1))
                    shift
                    ;;
                g)
                    case $(echo "$match" | cut -c$((i + 1))-) in
                    "")
                        # if it's -g Ninja
                        parse_options gen="$1"
                        i=$((i + 1))
                        shift
                        ;;
                    *)
                        # if it's -GNinja
                        # Just put everything past -g as the generator
                        parse_options gen="$(echo "$match" | cut -c$((i + 1))-)"
                        # go ahead and skip this block
                        i=$((${#match} + 1))
                        ;;
                    esac
                    ;;
                i)
                    parse_options install
                    i=$((i + 1))
                    ;;
                j)
                    case $(echo "$match" | cut -c$((i + 1))-) in
                    *[!0-9]*)
                        parse_options jobs="$1"
                        i=$((i + 1))
                        shift
                        ;;
                    *)
                        # Found number right after
                        parse_options jobs="$(echo "$match" | cut -c$((i + 1))-)"
                        # go ahead and skip this block
                        i=$((${#match} + 1))
                        ;;
                    esac
                    ;;
                p)
                    parse_options prefix="$1"
                    i=$((i + 1))
                    shift
                    ;;
                s)
                    parse_options target_system="$1"
                    i=$((i + 1))
                    shift
                    ;;
                t)
                    parse_options toolchain="$1"
                    i=$((i + 1))
                    shift
                    ;;
                x)
                    parse_options static
                    i=$((i + 1))
                    ;;
                v)
                    parse_options verbose
                    i=$((i + 1))
                    ;;
                *) die "Error, unknown option: -$(echo "$match" | cut -c$i | tr '[:upper:]' '[:lower:]')" ;;
                esac
            done
            ;;
        *) # Handle single word args
            match=$(printf %s "$1" | tr '[:upper:]' '[:lower:]')
            case $match in
            all) parse_options release debug && shift ;;
            asm=*) parse_options asm="${1#*=}" && shift ;;
            bindir=*) parse_options bindir="${1#*=}" && shift ;;
            cc=*) parse_options cc="${1#*=}" && shift ;;
            cxx=*) parse_options cxx="${1#*=}" && shift ;;
            c-only) parse_options c-only && shift ;;
            clean) parse_options clean && shift ;;
            debug) parse_options debug && shift ;;
            disable* | enable*) parse_options "$match" && shift ;;
            gen=*) parse_options gen="${1#*=}" && shift ;;
            help) parse_options help && shift ;;
            install) parse_options install && shift ;;
            jobs=*) parse_options jobs="${1#*=}" && shift ;;
            prefix=*) parse_options prefix="${1#*=}" && shift ;;
            pgo-dir=*) parse_options pgo-dir="${1#*=}" && shift ;;
            pgo-compile-gen) parse_options pgo-compile-gen && shift ;;
            pgo-compile-use) parse_options pgo-compile-use && shift ;;
            pgo-videos=*) parse_options pgo-videos="${1#*=}" && shift ;;
            no-apps) parse_options no-apps && shift ;;
            target_system=*) parse_options target_system="${1#*=}" && shift ;;
            android-ndk=*) parse_options android-ndk="${1#*=}" && shift ;;
            shared) parse_options shared && shift ;;
            static) parse_options static && shift ;;
            native) parse_options native && shift ;;
            release) parse_options release && shift ;;
            sanitizer=*) parse_options sanitizer="${1#*=}" && shift ;;
            test) parse_options tests && shift ;;
            toolchain=*) parse_options toolchain="${1#*=}" && shift ;;
            verbose) parse_options verbose && shift ;;
            minimal-build) parse_options minimal-build && shift ;;
            external-cpuinfo) parse_options external-cpuinfo && shift ;;
            end) ${IN_SCRIPT:-false} && exit ;;
            *) die "Error, unknown option: $1" ;;
            esac
            ;;
        esac
    done
fi

case $jobs in
*[!0-9]* | "") jobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null) ;;
esac

[ "${PATH#*\/usr\/local\/bin}" = "$PATH" ] && PATH=$PATH:/usr/local/bin

build_args='' compile_args=''

case $PGO_COMPILE_STAGE in
all) build_args='--target RunPGO' ;;
gen) build_args='--target PGOCompileGen' ;;
use) compile_args=no_clean build_args='--target PGOCompileUse' ;;
esac

$build_debug && {
    configure debug $compile_args "$@"
    build debug $build_args
}
if $build_release || ! $build_debug; then
    build_release=true
    configure release $compile_args "$@"
    build release $build_args
fi

if $build_install; then
    if $build_release; then
        install_build release
    else
        install_build debug
    fi
fi
