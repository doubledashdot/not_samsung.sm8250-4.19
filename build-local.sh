#!/usr/bin/env bash

SECONDS=0 # builtin bash timer

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Available models
VALID_MODELS=("bloomxq" "c1q" "c2q" "f2q" "gts7l" "gts7lwifi" "gts7xl" "gts7xlwifi" "r8q" "x1q" "y2q" "z3q")

VALID_ADDITIONALS=("stock" "ksu" "ksu+permissive")

# Prompt function
prompt() {
    echo "=============================================="
    echo "$1"
    echo "=============================================="
    shift
    for option in "$@"; do
        echo "$option"
    done
    echo "=============================================="
}

# Validate input
validate_choice() {
    local choice="$1"
    shift
    local valid_values=("$@")

    for value in "${valid_values[@]}"; do
        if [[ "$choice" == "$value" ]]; then
            return 0
        fi
    done

    return 1
}


# Model selection
prompt "Which project do you want to build?" "${VALID_MODELS[@]}"
read -p " - Enter your choice: " model_choice
model_choice=$(echo "$model_choice" | tr '[:upper:]' '[:lower:]')

if ! validate_choice "$model_choice" "${VALID_MODELS[@]}"; then
    echo "Invalid model choice! Exiting."
    exit 1
fi

# Aditional configs selection
prompt "Do you want to build with ksu/permissive?" "${VALID_ADDITIONALS[@]}"
read -p " - Enter your choice: " add_choice
add_choice=$(echo "$add_choice" | tr '[:upper:]' '[:lower:]')

if ! validate_choice "$add_choice" "${VALID_ADDITIONALS[@]}"; then
    echo "Invalid build add-on! Exiting."
    exit 1
fi

# Target properties
MODEL=$model_choice
export PROJECT_NAME="${MODEL}"
PROJECT_CONFIG="vendor/samsung/${MODEL}.config"

[ -z "${PLATFORM_VERSION}" ] && export PLATFORM_VERSION=11

ZIPNAME="$(date '+%Y%m%d').zip"
PLATFORM_DEFCONFIG="vendor/kona-perf_defconfig"
COMMON_DEFCONFIG="vendor/samsung/kona-sec-common.config"

# Additionals configurations switch
case "$add_choice" in
    stock)
        ZIPNAME="not-$ZIPNAME"
        EXTRA_CONFIG="vendor/not/localversion.config"
        ;;
    ksu)
        ZIPNAME="not-ksu-$ZIPNAME"
        EXTRA_CONFIG="vendor/not/ksu.config vendor/not/localversion.config"
        ;;
    ksu)
        ZIPNAME="not-ksu-permissive-$ZIPNAME"
        EXTRA_CONFIG="vendor/not/ksu.config vendor/not/localversion.config vendor/not/permissive.config"
        ;;
esac

DEFCONFIG="$PLATFORM_DEFCONFIG $COMMON_DEFCONFIG $PROJECT_CONFIG $EXTRA_CONFIG"


# Paths
AK3_REPO="https://github.com/notkernel-oss/AnyKernel3"
AK3_BRANCH="$MODEL"

TC_DIR="$(pwd)/tc/clang"
OUT_DIR="$(pwd)/out"
BOOT_DIR="$OUT_DIR/arch/arm64/boot"
DTS_DIR="$BOOT_DIR/dts/vendor/qcom"

# smth
if test -z "$(git rev-parse --show-cdup 2>/dev/null)" &&
   head=$(git rev-parse --verify HEAD 2>/dev/null); then
    ZIPNAME="${ZIPNAME::-4}-$(echo $head | cut -c1-8)-$MODEL.zip"
fi

git submodule update --init --recursive

export PATH="$TC_DIR/bin:$PATH"

# Toolchain setup
if ! [ -d "$TC_DIR" ]; then
    echo -e "${YELLOW}Clang not found! Downloading...${NC}"
    mkdir -p "$TC_DIR"

    ASSET_URL=$(
        curl -fsSL https://api.github.com/repos/Neutron-Toolchains/clang-build-catalogue/releases/latest |
        jq -r '.assets[]
            | select(.name | endswith(".tar.zst"))
            | .browser_download_url' |
        head -n1
    )

    if [ -z "$ASSET_URL" ]; then
        echo -e "${RED}Failed to find latest release!${NC}"
        exit 1
    fi

    if ! curl -L "$ASSET_URL" | tar --zstd -x -C "$TC_DIR" --strip-components=1; then
        echo -e "${RED}Download failed!${NC}"
        exit 1
    fi

    echo -e "${GREEN}Clang ready!${NC}"
fi

mkdir -p out

echo -e "${BLUE}Building with: $DEFCONFIG${NC}"

make O=out ARCH=arm64 $DEFCONFIG
make O=out ARCH=arm64 olddefconfig

# Common make flags
MAKE_ARGS="-j$(nproc --all) O=out ARCH=arm64 \
CC=clang LD=ld.lld AS=llvm-as AR=llvm-ar NM=llvm-nm \
OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
LLVM=1 LLVM_IAS=1"

echo -e "\n${GREEN}Starting compilation...${NC}"

rm -rf out/arch/arm64/boot #ok

make $MAKE_ARGS dtbo.img
make $MAKE_ARGS Image.gz


# Post build checks
if [ ! -f "$BOOT_DIR/Image.gz" ]; then
    echo -e "${RED}Compilation failed!${NC}"
    exit 1
fi

echo -e "${GREEN}Kernel Image found!${NC}"

if [ ! -d "$DTS_DIR" ]; then
    echo -e "${RED}DTS directory missing!${NC}"
    exit 1
fi

echo -e "${BLUE}Generating DTB...${NC}"
cat $(find "$DTS_DIR" -name "*.dtb" | sort) > "$BOOT_DIR/dtb"

if [ ! -f "$BOOT_DIR/dtb" ]; then
    echo -e "${RED}DTB generation failed!${NC}"
    exit 1
fi

echo -e "${GREEN}DTB + DTBO ready!${NC}"


# AK3
rm -rf AnyKernel3
echo "[*] Cloning AnyKernel3..."
git clone -q -b "$AK3_BRANCH" "$AK3_REPO" AnyKernel3 || exit 1

cp "$BOOT_DIR/dtbo.img" AnyKernel3/
cp "$BOOT_DIR/Image.gz" AnyKernel3/
cp "$BOOT_DIR/dtb" AnyKernel3/

# remove older builds
rm -rf *.zip

cd AnyKernel3
zip -r9 "../$ZIPNAME" * -x .git README.md *placeholder
cd ..

echo -e "\n${GREEN}Completed in $((SECONDS / 60))m $((SECONDS % 60))s${NC}"
echo -e "${GREEN}Zip: $ZIPNAME${NC}"
