#!/bin/bash
set -e

V8_BASE_REVISION=6a41721a2889b84cb2f3b920fbdc40b96347597a
BUILD_REVISION=adaab113d20dbac883ef911e55995fb6c8da9947
ICU_REVISION=297a4dd02b9d36c92ab9b4f121e433c9c3bc14f8

# predictable working directory
cd "$(dirname "$0")"

IS_LINUX=0
IS_MAC=0
IS_MINGW=0
unamestr="$(uname)"
if [[ "$unamestr" == 'Darwin' ]]; then
    IS_MAC=1
elif [[ "$unamestr" == 'Linux' ]]; then
    IS_LINUX=1
elif [[ "$unamestr" =~ MINGW ]]; then
    IS_MINGW=1
else
    echo "Unsupported operating system"
    exit 1
fi

if [[ "$IS_MAC" == 1 ]]; then
    SEDI=( "-i" "" )
else
    SEDI=( "-i" )
fi

if [[ ! -d depot_tools ]]; then
    function depothook {
        rm -rf depot_tools
    }
    trap depothook EXIT

    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git

    find depot_tools -maxdepth 1 -type f ! -iname '*.exe' ! -iname 'ninja-*' -exec  sed "${SEDI[@]}" -e "s/exec python /exec python2 /" '{}' \+
    sed "${SEDI[@]}" -e '/_PLATFORM_MAPPING = {/a\'$'\n'"  'msys': 'win'," depot_tools/gclient.py
    sed "${SEDI[@]}" -e '/_PLATFORM_MAPPING = {/a\'$'\n'"  'msys': 'win'," depot_tools/gclient.py
    sed "${SEDI[@]}" -e '/ DEPS_OS_CHOICES = {/a\'$'\n'"    'msys': 'win'," depot_tools/gclient.py
    sed "${SEDI[@]}" -e '/PLATFORM_MAPPING = {/a\'$'\n'"    'msys': 'win'," depot_tools/download_from_google_storage.py
    sed "${SEDI[@]}" -e "s/  if sys.platform == 'cygwin':/  if sys.platform in ('cygwin', 'msys'):/" depot_tools/download_from_google_storage.py
    sed "${SEDI[@]}" -e "s/  if sys.platform.startswith(('cygwin', 'win')):/  if sys.platform.startswith(('cygwin', 'win', 'msys')):/" depot_tools/gclient_utils.py

    # initialize depot_tools checkout
    cd depot_tools
    ./gclient > /dev/null
    cd ..
    
    trap '-' EXIT
fi

export PATH=$PATH:$PWD/depot_tools

if [[ ! -d v8 ]]; then
    function v8hook {
        rm -rf v8 .gclient .gclient_entries
    }
    trap v8hook EXIT

    fetch --nohooks v8

    trap '-' EXIT
fi

cd v8

if [[ "$IS_MINGW" == 1 ]]; then
    if [[ ! -e .patched || "$(cat .patched)" != "$V8_BASE_REVISION" ]]; then
        git checkout $V8_BASE_REVISION
        git am ../patches/0001-mingw-build.patch
        # only run gclient once on mingw as it's rather slow
        gclient sync
        printf "$V8_BASE_REVISION" > .patched
    fi

    cd build
    if [[ ! -e .patched || "$(cat .patched)" != "$V8_BASE_REVISION" ]]; then
        git checkout $BUILD_REVISION
        git am ../../patches/0001-build-mingw-build.patch
        printf "$V8_BASE_REVISION" > .patched
    fi

    cd ../third_party/icu
    if [[ ! -e .patched || "$(cat .patched)" != "$V8_BASE_REVISION" ]]; then
        git checkout $ICU_REVISION
        git am ../../../patches/0001-icu-mingw-build.patch
        printf "$V8_BASE_REVISION" > .patched
    fi
    cd ../..
else
    if [[ "$(git show-ref -s --verify HEAD)" != "$V8_BASE_REVISION" ]]; then
        git checkout $V8_BASE_REVISION
    fi
    gclient sync
fi

if [[ "$IS_LINUX" == 1 ]]; then
    ./build/install-build-deps.sh --no-arm --no-nacl
fi

if [[ "$IS_MINGW" == 1 ]]; then
    mkdir -p out/x86.release
    gn gen out/x86.release --args="is_debug=false target_cpu=\"x86\" is_component_build=false v8_static_library=true use_custom_libcxx=false use_custom_libcxx_for_host=false custom_toolchain=\"//build/toolchain/win:gcc_x86\" is_clang=false treat_warnings_as_errors=false"
    ninja -C out/x86.release
else
    mkdir -p out/x64.release
    gn gen out/x64.release --args="is_debug=false target_cpu=\"x64\" is_component_build=false v8_static_library=true use_custom_libcxx=false use_custom_libcxx_for_host=false"
    ninja -C out/x64.release
fi
