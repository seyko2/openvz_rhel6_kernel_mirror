#
# Arch-specific CryptoAPI modules.
#

obj-$(CONFIG_CRYPTO_ABLK_HELPER_X86) += ablk_helper.o
obj-$(CONFIG_CRYPTO_GLUE_HELPER_X86) += glue_helper.o

obj-$(CONFIG_CRYPTO_AES_586) += aes-i586.o
obj-$(CONFIG_CRYPTO_TWOFISH_586) += twofish-i586.o
obj-$(CONFIG_CRYPTO_SALSA20_586) += salsa20-i586.o

obj-$(CONFIG_CRYPTO_AES_X86_64) += aes-x86_64.o
obj-$(CONFIG_CRYPTO_TWOFISH_X86_64) += twofish-x86_64.o
obj-$(CONFIG_CRYPTO_SALSA20_X86_64) += salsa20-x86_64.o
obj-$(CONFIG_CRYPTO_AES_NI_INTEL) += aesni-intel.o
obj-$(CONFIG_CRYPTO_GHASH_CLMUL_NI_INTEL) += ghash-clmulni-intel.o

obj-$(CONFIG_CRYPTO_CRC32C_INTEL) += crc32c-intel.o
obj-$(CONFIG_CRYPTO_SHA1_SSSE3) += sha1-ssse3.o

obj-$(CONFIG_CRYPTO_SHA256_SSSE3) += sha256-ssse3.o
obj-$(CONFIG_CRYPTO_SHA512_SSSE3) += sha512-ssse3.o
obj-$(CONFIG_CRYPTO_CRCT10DIF_PCLMUL) += crct10dif-pclmul.o

aes-i586-y := aes-i586-asm_32.o aes_glue.o
twofish-i586-y := twofish-i586-asm_32.o twofish_glue.o
salsa20-i586-y := salsa20-i586-asm_32.o salsa20_glue.o

aes-x86_64-y := aes-x86_64-asm_64.o aes_glue.o
twofish-x86_64-y := twofish-x86_64-asm_64.o twofish_glue.o
salsa20-x86_64-y := salsa20-x86_64-asm_64.o salsa20_glue.o

aesni-intel-y := aesni-intel_asm.o aesni-intel_glue.o fpu.o

ghash-clmulni-intel-y := ghash-clmulni-intel_asm.o ghash-clmulni-intel_glue.o
crc32c-intel-y := crc32c-intel_glue.o
crc32c-intel-$(CONFIG_64BIT) += crc32c-pcl-intel-asm_64.o

# enable AVX support only when $(AS) can actually assemble the instructions
ifeq ($(call as-instr,vpxor %xmm0$(comma)%xmm1$(comma)%xmm2,yes,no),yes)
AFLAGS_sha1_ssse3_asm.o += -DSHA1_ENABLE_AVX_SUPPORT
CFLAGS_sha1_ssse3_glue.o += -DSHA1_ENABLE_AVX_SUPPORT
endif
sha1-ssse3-y := sha1_ssse3_asm.o sha1_ssse3_glue.o

crc32-pclmul-y := crc32-pclmul_asm.o crc32-pclmul_glue.o
sha256-ssse3-y := sha256-ssse3-asm.o sha256-avx-asm.o sha256-avx2-asm.o sha256_ssse3_glue.o
sha512-ssse3-y := sha512-ssse3-asm.o sha512-avx-asm.o sha512-avx2-asm.o sha512_ssse3_glue.o
crct10dif-pclmul-y := crct10dif-pcl-asm_64.o crct10dif-pclmul_glue.o
