#include "yati/yati.hpp"
#include "yati/source/file.hpp"
#include "yati/source/stdio.hpp"
#include "yati/container/nsp.hpp"
#include "yati/container/xci.hpp"

#include "yati/nx/ncz.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/ns.hpp"
#include "yati/nx/es.hpp"
#include "yati/nx/keys.hpp"
#include "yati/nx/crypto.hpp"

#include "ui/progress_box.hpp"
#include "app.hpp"
#include "i18n.hpp"
#include "log.hpp"

#include <new>
#include <zstd.h>
#include <minIni.h>

namespace sphaira::yati {
namespace {

constexpr NcmStorageId NCM_STORAGE_IDS[]{
    NcmStorageId_BuiltInUser,
    NcmStorageId_SdCard,
};

// custom allocator for std::vector that respects alignment.
// https://en.cppreference.com/w/cpp/named_req/Allocator
template <typename T, std::size_t Align>
struct CustomVectorAllocator {
public:
    // https://en.cppreference.com/w/cpp/memory/new/operator_new
    auto allocate(std::size_t n) -> T* {
        log_write("allocating ptr size: %zu\n", n);
        return new(align) T[n];
    }

    // https://en.cppreference.com/w/cpp/memory/new/operator_delete
    auto deallocate(T* p, std::size_t n) noexcept -> void {
        log_write("deleting ptr size: %zu\n", n);
        ::operator delete[] (p, n, align);
    }

private:
    static constexpr inline std::align_val_t align{Align};
};

template <typename T>
struct PageAllocator : CustomVectorAllocator<T, 0x1000> {
    using value_type = T; // used by std::vector
};

template<class T, class U>
bool operator==(const PageAllocator <T>&, const PageAllocator <U>&) { return true; }

using PageAlignedVector = std::vector<u8, PageAllocator<u8>>;

constexpr u32 KEYGEN_LIMIT = 0x20;

#if 0
struct FwVersion {
    u32 value;
    auto relstep() const -> u8 { return (value >> 0)  & 0xFFFF; }
    auto micro() const -> u8 { return (value >> 16) & 0x000F; }
    auto minor() const -> u8 { return (value >> 20) & 0x003F; }
    auto major() const -> u8 { return (value >> 26) & 0x003F; }
    auto hos() const -> u32 { return MAKEHOSVERSION(major(), minor(), micro()); }
};
#endif

struct NcaCollection : container::CollectionEntry {
    // NcmContentType
    u8 type{};
    NcmContentId content_id{};
    NcmPlaceHolderId placeholder_id{};
    // new hash of the nca..
    u8 hash[SHA256_HASH_SIZE]{};
    // set true if nca has been modified.
    bool modified{};
};

struct CnmtCollection : NcaCollection {
    // list of all nca's the cnmt depends on
    std::vector<NcaCollection> ncas{};
    // only set if any of the nca's depend on a ticket / cert.
    // if set, the ticket / cert will be installed once all nca's have installed.
    std::vector<FsRightsId> rights_id{};

    NcmContentMetaHeader header{};
    NcmContentMetaKey key{};
    NcmContentInfo content_info{};
    std::vector<u8> extended_header{};
    std::vector<NcmPackagedContentInfo> infos{};
};

struct TikCollection {
    // raw data of the ticket / cert.
    std::vector<u8> ticket{};
    std::vector<u8> cert{};
    // set via the name of the ticket.
    FsRightsId rights_id{};
    // set if ticket is required by an nca.
    bool required{};
};

struct Yati;

const u64 INFLATE_BUFFER_MAX = 1024*1024*4;

struct ThreadBuffer {
    ThreadBuffer() {
        buf.reserve(INFLATE_BUFFER_MAX);
    }

    PageAlignedVector buf;
    s64 off;
};

template<std::size_t Size>
struct RingBuf {
private:
    ThreadBuffer buf[Size]{};
    unsigned r_index{};
    unsigned w_index{};

    static_assert((sizeof(RingBuf::buf) & (sizeof(RingBuf::buf) - 1)) == 0, "Must be power of 2!");

public:
    void ringbuf_reset() {
        this->r_index = this->w_index;
    }

    unsigned ringbuf_capacity() const {
        return sizeof(this->buf) / sizeof(this->buf[0]);
    }

    unsigned ringbuf_size() const {
        return (this->w_index - this->r_index) % (ringbuf_capacity() * 2U);
    }

    unsigned ringbuf_free() const {
        return ringbuf_capacity() - ringbuf_size();
    }

    void ringbuf_push(PageAlignedVector& buf_in, s64 off_in) {
        auto& value = this->buf[this->w_index % ringbuf_capacity()];
        value.off = off_in;
        std::swap(value.buf, buf_in);

        this->w_index = (this->w_index + 1U) % (ringbuf_capacity() * 2U);
    }

    void ringbuf_pop(PageAlignedVector& buf_out, s64& off_out) {
        auto& value = this->buf[this->r_index % ringbuf_capacity()];
        off_out = value.off;
        std::swap(value.buf, buf_out);

        this->r_index = (this->r_index + 1U) % (ringbuf_capacity() * 2U);
    }
};

struct ThreadData {
    ThreadData(Yati* _yati, std::span<TikCollection> _tik, NcaCollection* _nca)
    : yati{_yati}, tik{_tik}, nca{_nca} {
        mutexInit(std::addressof(read_mutex));
        mutexInit(std::addressof(write_mutex));

        condvarInit(std::addressof(can_read));
        condvarInit(std::addressof(can_decompress));
        condvarInit(std::addressof(can_decompress_write));
        condvarInit(std::addressof(can_write));

        sha256ContextCreate(&sha256);
        // this will be updated with the actual size from nca header.
        write_size = nca->size;

        read_buffer_size = 1024*1024*4;
        max_buffer_size = std::max(read_buffer_size, INFLATE_BUFFER_MAX);
    }

    auto GetResults() -> Result;
    void WakeAllThreads();

    Result Read(void* buf, s64 size, u64* bytes_read);

    Result SetDecompressBuf(PageAlignedVector& buf, s64 off, s64 size) {
        buf.resize(size);

        mutexLock(std::addressof(read_mutex));
        if (!read_buffers.ringbuf_free()) {
            R_TRY(condvarWait(std::addressof(can_read), std::addressof(read_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(read_mutex)));
        R_TRY(GetResults());
        read_buffers.ringbuf_push(buf, off);
        return condvarWakeOne(std::addressof(can_decompress));
    }

    Result GetDecompressBuf(PageAlignedVector& buf_out, s64& off_out) {
        mutexLock(std::addressof(read_mutex));
        if (!read_buffers.ringbuf_size()) {
            R_TRY(condvarWait(std::addressof(can_decompress), std::addressof(read_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(read_mutex)));
        R_TRY(GetResults());
        read_buffers.ringbuf_pop(buf_out, off_out);
        return condvarWakeOne(std::addressof(can_read));
    }

    Result SetWriteBuf(PageAlignedVector& buf, s64 size, bool skip_verify) {
        buf.resize(size);
        if (!skip_verify) {
            sha256ContextUpdate(std::addressof(sha256), buf.data(), buf.size());
        }

        mutexLock(std::addressof(write_mutex));
        if (!write_buffers.ringbuf_free()) {
            R_TRY(condvarWait(std::addressof(can_decompress_write), std::addressof(write_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(write_mutex)));
        R_TRY(GetResults());
        write_buffers.ringbuf_push(buf, 0);
        return condvarWakeOne(std::addressof(can_write));
    }

    Result GetWriteBuf(PageAlignedVector& buf_out, s64& off_out) {
        mutexLock(std::addressof(write_mutex));
        if (!write_buffers.ringbuf_size()) {
            R_TRY(condvarWait(std::addressof(can_write), std::addressof(write_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(write_mutex)));
        R_TRY(GetResults());
        write_buffers.ringbuf_pop(buf_out, off_out);
        return condvarWakeOne(std::addressof(can_decompress_write));
    }

    // these need to be copied
    Yati* yati{};
    std::span<TikCollection> tik{};
    NcaCollection* nca{};

    // these need to be created
    Mutex read_mutex{};
    Mutex write_mutex{};

    CondVar can_read{};
    CondVar can_decompress{};
    CondVar can_decompress_write{};
    CondVar can_write{};

    RingBuf<4> read_buffers{};
    RingBuf<4> write_buffers{};

    ncz::BlockHeader ncz_block_header{};
    std::vector<ncz::Section> ncz_sections{};
    std::vector<ncz::BlockInfo> ncz_blocks{};

    Sha256Context sha256{};

    u64 read_buffer_size{};
    u64 max_buffer_size{};

    // these are shared between threads
    volatile s64 read_offset{};
    volatile s64 decompress_offset{};
    volatile s64 write_offset{};
    volatile s64 write_size{};

    volatile Result read_result{};
    volatile Result decompress_result{};
    volatile Result write_result{};
};

struct Yati {
    Yati(ui::ProgressBox*, std::shared_ptr<source::Base>);
    ~Yati();

    Result Setup();
    Result InstallNca(std::span<TikCollection> tickets, NcaCollection& nca);
    Result InstallCnmtNca(std::span<TikCollection> tickets, CnmtCollection& cnmt, const container::Collections& collections);
    Result InstallControlNca(std::span<TikCollection> tickets, const CnmtCollection& cnmt, NcaCollection& nca);

    Result readFuncInternal(ThreadData* t);
    Result decompressFuncInternal(ThreadData* t);
    Result writeFuncInternal(ThreadData* t);

// private:
    ui::ProgressBox* pbox{};
    std::shared_ptr<source::Base> source{};

    // for all content storages
    NcmContentStorage ncm_cs[2]{};
    NcmContentMetaDatabase ncm_db[2]{};
    // these point to the above struct
    NcmContentStorage cs{};
    NcmContentMetaDatabase db{};
    NcmStorageId storage_id{};

    Service es{};
    Service ns_app{};
    std::unique_ptr<container::Base> container{};
    Config config{};
    keys::Keys keys{};
};

auto ThreadData::GetResults() -> Result {
    R_UNLESS(!yati->pbox->ShouldExit(), Result_Cancelled);
    R_TRY(read_result);
    R_TRY(decompress_result);
    R_TRY(write_result);
    R_SUCCEED();
}

void ThreadData::WakeAllThreads() {
    condvarWakeAll(std::addressof(can_read));
    condvarWakeAll(std::addressof(can_decompress));
    condvarWakeAll(std::addressof(can_decompress_write));
    condvarWakeAll(std::addressof(can_write));

    mutexUnlock(std::addressof(read_mutex));
    mutexUnlock(std::addressof(write_mutex));
}

Result ThreadData::Read(void* buf, s64 size, u64* bytes_read) {
    size = std::min<s64>(size, nca->size - read_offset);
    const auto rc = yati->source->Read(buf, nca->offset + read_offset, size, bytes_read);
    read_offset += *bytes_read;
    R_UNLESS(size == *bytes_read, Result_InvalidNcaReadSize);
    return rc;
}

auto isRightsIdValid(FsRightsId id) -> bool {
    FsRightsId empty_id{};
    return 0 != std::memcmp(std::addressof(id), std::addressof(empty_id), sizeof(id));
}

auto getKeyGenFromRightsId(FsRightsId id) -> u8 {
    return id.c[sizeof(id) - 1];
}

struct HashStr {
    char str[0x21];
};

HashStr hexIdToStr(auto id) {
    HashStr str{};
    const auto id_lower = std::byteswap(*(u64*)id.c);
    const auto id_upper = std::byteswap(*(u64*)(id.c + 0x8));
    std::snprintf(str.str, 0x21, "%016lx%016lx", id_lower, id_upper);
    return str;
}

// read thread reads all data from the source, it also handles
// parsing ncz headers, sections and reading ncz blocks
Result Yati::readFuncInternal(ThreadData* t) {
    PageAlignedVector buf;
    buf.reserve(t->max_buffer_size);

    while (t->read_offset < t->nca->size && R_SUCCEEDED(t->GetResults())) {
        const auto buffer_offset = t->read_offset;

        // read more data
        s64 read_size = t->read_buffer_size;
        if (!t->read_offset) {
            read_size = NCZ_SECTION_OFFSET;
        }

        u64 bytes_read{};
        buf.resize(read_size);
        R_TRY(t->Read(buf.data(), read_size, std::addressof(bytes_read)));
        auto buf_size = bytes_read;

        // read enough bytes for ncz, check magic
        if (t->read_offset == NCZ_SECTION_OFFSET) {
            // check for ncz section header.
            ncz::Header header{};
            std::memcpy(std::addressof(header), buf.data() + 0x4000, sizeof(header));
            if (header.magic == NCZ_SECTION_MAGIC) {
                // validate section header.
                R_UNLESS(header.total_sections, Result_InvalidNczSectionCount);

                buf_size = 0x4000;
                log_write("found ncz, total number of sections: %zu\n", header.total_sections);
                t->ncz_sections.resize(header.total_sections);
                R_TRY(t->Read(t->ncz_sections.data(), t->ncz_sections.size() * sizeof(ncz::Section), std::addressof(bytes_read)));

                // check for ncz block header.
                const auto read_off = t->read_offset;
                R_TRY(t->Read(std::addressof(t->ncz_block_header), sizeof(t->ncz_block_header), std::addressof(bytes_read)));
                if (t->ncz_block_header.magic != NCZ_BLOCK_MAGIC) {
                    t->read_offset = read_off;
                } else {
                    // validate block header.
                    R_UNLESS(t->ncz_block_header.version == 0x2, Result_InvalidNczBlockVersion);
                    R_UNLESS(t->ncz_block_header.type == 0x1, Result_InvalidNczBlockType);
                    R_UNLESS(t->ncz_block_header.total_blocks, Result_InvalidNczBlockTotal);
                    R_UNLESS(t->ncz_block_header.block_size_exponent >= 14 && t->ncz_block_header.block_size_exponent <= 32, Result_InvalidNczBlockSizeExponent);

                    // read blocks (array of block sizes).
                    std::vector<ncz::Block> blocks(t->ncz_block_header.total_blocks);
                    R_TRY(t->Read(blocks.data(), blocks.size() * sizeof(ncz::Block), std::addressof(bytes_read)));

                    // calculate offsets for each block.
                    auto block_offset = t->read_offset;
                    for (const auto& block : blocks) {
                        t->ncz_blocks.emplace_back(block_offset, block.size);
                        block_offset += block.size;
                    }
                }
            }
        }

        R_TRY(t->SetDecompressBuf(buf, buffer_offset, buf_size));
    }

    log_write("read success\n");
    R_SUCCEED();
}

// decompress thread handles decrypting / modifying the nca header, decompressing ncz
// and calculating the running sha256.
Result Yati::decompressFuncInternal(ThreadData* t) {
    // only used for ncz files.
    auto dctx = ZSTD_createDCtx();
    ON_SCOPE_EXIT(ZSTD_freeDCtx(dctx));
    const auto chunk_size = ZSTD_DStreamOutSize();
    const ncz::Section* ncz_section{};
    const ncz::BlockInfo* ncz_block{};
    bool is_ncz{};

    s64 inflate_offset{};
    Aes128CtrContext ctx{};
    PageAlignedVector inflate_buf{};
    inflate_buf.reserve(t->max_buffer_size);

    s64 written{};
    s64 decompress_buf_off{};
    PageAlignedVector buf{};
    buf.reserve(t->max_buffer_size);

    // encrypts the nca and passes the buffer to the write thread.
    const auto ncz_flush = [&](s64 size) -> Result {
        if (!inflate_offset) {
            R_SUCCEED();
        }

        // if we are not moving the whole vector, then we need to keep
        // the remaining data.
        // rather that copying the entire vector to the write thread,
        // only copy (store) the remaining amount.
        PageAlignedVector temp_vector{};
        if (size < inflate_offset) {
            temp_vector.resize(inflate_offset - size);
            std::memcpy(temp_vector.data(), inflate_buf.data() + size, temp_vector.size());
        }

        for (s64 off = 0; off < size;) {
            // log_write("looking for section\n");
            if (!ncz_section || !ncz_section->InRange(written)) {
                auto it = std::find_if(t->ncz_sections.cbegin(), t->ncz_sections.cend(), [written](auto& e){
                    return e.InRange(written);
                });

                R_UNLESS(it != t->ncz_sections.cend(), Result_NczSectionNotFound);
                ncz_section = &(*it);

                if (ncz_section->crypto_type >= nca::EncryptionType_AesCtr) {
                    const auto swp = std::byteswap(u64(written) >> 4);
                    u8 counter[0x16];
                    std::memcpy(counter + 0x0, ncz_section->counter, 0x8);
                    std::memcpy(counter + 0x8, &swp, 0x8);
                    aes128CtrContextCreate(&ctx, ncz_section->key, counter);
                }
            }

            const auto total_size = ncz_section->offset + ncz_section->size;
            const auto chunk_size = std::min<u64>(total_size - written, size - off);

            if (ncz_section->crypto_type >= nca::EncryptionType_AesCtr) {
                aes128CtrCrypt(&ctx, inflate_buf.data() + off, inflate_buf.data() + off, chunk_size);
            }

            written += chunk_size;
            off += chunk_size;
        }

        R_TRY(t->SetWriteBuf(inflate_buf, size, config.skip_nca_hash_verify));
        inflate_offset -= size;

        // restore remaining data to the swapped buffer.
        if (!temp_vector.empty()) {
            log_write("storing data size: %zu\n", temp_vector.size());
            inflate_buf = temp_vector;
        }

        R_SUCCEED();
    };

    while (t->decompress_offset < t->write_size && R_SUCCEEDED(t->GetResults())) {
        R_TRY(t->GetDecompressBuf(buf, decompress_buf_off));

        // do we have an nsz? if so, setup buffers.
        if (!is_ncz && !t->ncz_sections.empty()) {
            log_write("YES IT FOUND NCZ\n");
            is_ncz = true;
        }

        // if we don't have a ncz or it's before the ncz header, pass buffer directly to write
        if (!is_ncz || !decompress_buf_off) {
            // check nca header
            if (!decompress_buf_off) {
                nca::Header header{};
                crypto::cryptoAes128Xts(buf.data(), std::addressof(header), keys.header_key, 0, 0x200, sizeof(header), false);
                log_write("verifying nca header magic\n");
                R_UNLESS(header.magic == 0x3341434E, Result_InvalidNcaMagic);
                log_write("nca magic is ok! type: %u\n", header.content_type);

                if (!config.skip_rsa_header_fixed_key_verify) {
                    log_write("verifying nca fixed key\n");
                    R_TRY(nca::VerifyFixedKey(header));
                    log_write("nca fixed key is ok! type: %u\n", header.content_type);
                } else {
                    log_write("skipping nca verification\n");
                }

                t->write_size = header.size;
                R_TRY(ncmContentStorageSetPlaceHolderSize(std::addressof(cs), std::addressof(t->nca->placeholder_id), header.size));

                if (header.distribution_type == nca::DistributionType_GameCard) {
                    header.distribution_type = nca::DistributionType_System;
                    t->nca->modified = true;
                }

                TikCollection* ticket = nullptr;
                if (isRightsIdValid(header.rights_id)) {
                    auto it = std::find_if(t->tik.begin(), t->tik.end(), [header](auto& e){
                        return !std::memcmp(&header.rights_id, &e.rights_id, sizeof(e.rights_id));
                    });

                    R_UNLESS(it != t->tik.end(), Result_TicketNotFound);
                    it->required = true;
                    ticket = &(*it);
                }

                if ((config.convert_to_standard_crypto && isRightsIdValid(header.rights_id)) || config.lower_master_key) {
                    t->nca->modified = true;
                    u8 keak_generation;

                    if (isRightsIdValid(header.rights_id)) {
                        const auto key_gen = getKeyGenFromRightsId(header.rights_id);
                        log_write("converting to standard crypto: 0x%X 0x%X\n", key_gen, header.key_gen);

                        // fetch ticket data block.
                        es::TicketData ticket_data;
                        R_TRY(es::GetTicketData(ticket->ticket, std::addressof(ticket_data)));

                        // validate that this indeed the correct ticket.
                        R_UNLESS(!std::memcmp(std::addressof(header.rights_id), std::addressof(ticket_data.rights_id), sizeof(header.rights_id)), Result_InvalidTicketBadRightsId);

                        // some scene releases use buggy software which set the master key
                        // revision in the properties bitfield...lol, still happens in 2025.
                        // to fix this, get mkey rev from the rights id
                        // todo: verify this code.
                        if (ticket_data.title_key_type == es::TicketTitleKeyType_Common) {
                            if (!ticket_data.master_key_revision && ticket_data.master_key_revision != getKeyGenFromRightsId(ticket_data.rights_id) && ticket_data.properties_bitfield) {
                                // get the actual mkey
                                ticket_data.master_key_revision = getKeyGenFromRightsId(ticket_data.rights_id);
                                // unset the properties
                                ticket_data.properties_bitfield = 0;
                            }
                        }

                        // decrypt title key.
                        keys::KeyEntry title_key;
                        R_TRY(es::GetTitleKey(title_key, ticket_data, keys));
                        R_TRY(es::DecryptTitleKey(title_key, key_gen, keys));

                        std::memset(header.key_area, 0, sizeof(header.key_area));
                        std::memcpy(&header.key_area[0x2], &title_key, sizeof(title_key));

                        keak_generation = key_gen;
                        ticket->required = false;
                    } else if (config.lower_master_key) {
                        R_TRY(nca::DecryptKeak(keys, header));
                    }

                    if (config.lower_master_key) {
                        keak_generation = 0;
                    }

                    R_TRY(nca::EncryptKeak(keys, header, keak_generation));
                    std::memset(&header.rights_id, 0, sizeof(header.rights_id));
                }

                if (t->nca->modified) {
                    crypto::cryptoAes128Xts(std::addressof(header), buf.data(), keys.header_key, 0, 0x200, sizeof(header), true);
                }
            }

            written += buf.size();
            t->decompress_offset += buf.size();
            R_TRY(t->SetWriteBuf(buf, buf.size(), config.skip_nca_hash_verify));
        } else if (is_ncz) {
            u64 buf_off{};
            while (buf_off < buf.size()) {
                std::span<const u8> buffer{buf.data() + buf_off, buf.size() - buf_off};
                bool compressed = true;

                // todo: blocks need to use read offset, as the offset + size is compressed range.
                if (t->ncz_blocks.size()) {
                    if (!ncz_block || !ncz_block->InRange(decompress_buf_off)) {
                        auto it = std::find_if(t->ncz_blocks.cbegin(), t->ncz_blocks.cend(), [decompress_buf_off](auto& e){
                            return e.InRange(decompress_buf_off);
                        });

                        R_UNLESS(it != t->ncz_blocks.cend(), Result_NczBlockNotFound);
                        // log_write("looking found block\n");
                        ncz_block = &(*it);
                    }

                    // https://github.com/nicoboss/nsz/issues/79
                    auto decompressedBlockSize = 1 << t->ncz_block_header.block_size_exponent;
                    // special handling for the last block to check it's actually compressed
                    if (ncz_block->offset == t->ncz_blocks.back().offset) {
                        log_write("last block special handling\n");
                        decompressedBlockSize = t->ncz_block_header.decompressed_size % decompressedBlockSize;
                    }

                    // check if this block is compressed.
                    compressed = ncz_block->size < decompressedBlockSize;

                    // clip read size as blocks can be up to 32GB in size!
                    const auto size = std::min<u64>(buf.size() - buf_off, ncz_block->size);
                    buffer = {buf.data() + buf_off, size};
                }

                if (compressed) {
                    // log_write("COMPRESSED block\n");
                    ZSTD_inBuffer input = { buffer.data(), buffer.size(), 0 };
                    while (input.pos < input.size) {
                        R_TRY(t->GetResults());

                        inflate_buf.resize(inflate_offset + chunk_size);
                        ZSTD_outBuffer output = { inflate_buf.data() + inflate_offset, chunk_size, 0 };
                        const auto res = ZSTD_decompressStream(dctx, std::addressof(output), std::addressof(input));
                        R_UNLESS(!ZSTD_isError(res), Result_InvalidNczZstdError);

                        t->decompress_offset += output.pos;
                        inflate_offset += output.pos;
                        if (inflate_offset >= INFLATE_BUFFER_MAX) {
                            // log_write("flushing compressed data: %zd vs %zd diff: %zd\n", inflate_offset, INFLATE_BUFFER_MAX, inflate_offset - INFLATE_BUFFER_MAX);
                            R_TRY(ncz_flush(INFLATE_BUFFER_MAX));
                        }
                    }
                } else {
                    inflate_buf.resize(inflate_offset + buffer.size());
                    std::memcpy(inflate_buf.data() + inflate_offset, buffer.data(), buffer.size());

                    t->decompress_offset += buffer.size();
                    inflate_offset += buffer.size();
                    if (inflate_offset >= INFLATE_BUFFER_MAX) {
                        // log_write("flushing copy data\n");
                        R_TRY(ncz_flush(INFLATE_BUFFER_MAX));
                    }
                }

                buf_off += buffer.size();
                decompress_buf_off += buffer.size();
            }
        }
    }

    // flush remaining data.
    if (is_ncz && inflate_offset) {
        log_write("flushing remaining\n");
        R_TRY(ncz_flush(inflate_offset));
    }

    log_write("decompress thread done!\n");

    // get final hash output.
    sha256ContextGetHash(std::addressof(t->sha256), t->nca->hash);

    R_SUCCEED();
}

// write thread writes data to the nca placeholder.
Result Yati::writeFuncInternal(ThreadData* t) {
    PageAlignedVector buf;
    buf.reserve(t->max_buffer_size);

    while (t->write_offset < t->write_size && R_SUCCEEDED(t->GetResults())) {
        s64 dummy_off;
        R_TRY(t->GetWriteBuf(buf, dummy_off));
        R_TRY(ncmContentStorageWritePlaceHolder(std::addressof(cs), std::addressof(t->nca->placeholder_id), t->write_offset, buf.data(), buf.size()));
        t->write_offset += buf.size();
    }

    log_write("finished write thread!\n");
    R_SUCCEED();
}

void readFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->read_result = t->yati->readFuncInternal(t);
    log_write("read thread returned now\n");
}

void decompressFunc(void* d) {
    log_write("hello decomp thread func\n");
    auto t = static_cast<ThreadData*>(d);
    t->decompress_result = t->yati->decompressFuncInternal(t);
    log_write("decompress thread returned now\n");
}

void writeFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->write_result = t->yati->writeFuncInternal(t);
    log_write("write thread returned now\n");
}

// stdio-like wrapper for std::vector
struct BufHelper {
    BufHelper() = default;
    BufHelper(std::span<const u8> data) {
        write(data);
    }

    void write(const void* data, u64 size) {
        if (offset + size >= buf.size()) {
            buf.resize(offset + size);
        }
        std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }

    void write(std::span<const u8> data) {
        write(data.data(), data.size());
    }

    void seek(u64 where_to) {
        offset = where_to;
    }

    [[nodiscard]]
    auto tell() const {
        return offset;
    }

    std::vector<u8> buf{};
    u64 offset{};
};

Yati::Yati(ui::ProgressBox* _pbox, std::shared_ptr<source::Base> _source) : pbox{_pbox}, source{_source} {
    appletSetMediaPlaybackState(true);
}

Yati::~Yati() {
    splCryptoExit();
    serviceClose(std::addressof(ns_app));
    nsExit();

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        ncmContentMetaDatabaseClose(std::addressof(ncm_db[i]));
        ncmContentStorageClose(std::addressof(ncm_cs[i]));
    }

    serviceClose(std::addressof(es));
    appletSetMediaPlaybackState(false);
}

Result Yati::Setup() {
    config.sd_card_install = App::GetApp()->m_install_sd.Get();
    config.allow_downgrade = App::GetApp()->m_allow_downgrade.Get();
    config.skip_if_already_installed = App::GetApp()->m_skip_if_already_installed.Get();
    config.ticket_only = App::GetApp()->m_ticket_only.Get();
    config.patch_ticket = App::GetApp()->m_patch_ticket.Get();
    config.skip_base = App::GetApp()->m_skip_base.Get();
    config.skip_patch = App::GetApp()->m_skip_patch.Get();
    config.skip_addon = App::GetApp()->m_skip_addon.Get();
    config.skip_data_patch = App::GetApp()->m_skip_data_patch.Get();
    config.skip_ticket = App::GetApp()->m_skip_ticket.Get();
    config.skip_nca_hash_verify = App::GetApp()->m_skip_nca_hash_verify.Get();
    config.skip_rsa_header_fixed_key_verify = App::GetApp()->m_skip_rsa_header_fixed_key_verify.Get();
    config.skip_rsa_npdm_fixed_key_verify = App::GetApp()->m_skip_rsa_npdm_fixed_key_verify.Get();
    config.ignore_distribution_bit = App::GetApp()->m_ignore_distribution_bit.Get();
    config.convert_to_standard_crypto = App::GetApp()->m_convert_to_standard_crypto.Get();
    config.lower_master_key = App::GetApp()->m_lower_master_key.Get();
    config.lower_system_version = App::GetApp()->m_lower_system_version.Get();
    storage_id = config.sd_card_install ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    R_TRY(source->GetOpenResult());
    R_TRY(splCryptoInitialize());
    R_TRY(nsInitialize());
    R_TRY(nsGetApplicationManagerInterface(std::addressof(ns_app)));
    R_TRY(smGetService(std::addressof(es), "es"));

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        R_TRY(ncmOpenContentMetaDatabase(std::addressof(ncm_db[i]), NCM_STORAGE_IDS[i]));
        R_TRY(ncmOpenContentStorage(std::addressof(ncm_cs[i]), NCM_STORAGE_IDS[i]));
    }

    cs = ncm_cs[config.sd_card_install];
    db = ncm_db[config.sd_card_install];

    R_TRY(parse_keys(keys, true));
    R_SUCCEED();
}

Result Yati::InstallNca(std::span<TikCollection> tickets, NcaCollection& nca) {
    log_write("in install nca\n");
    pbox->NewTransfer(nca.name);
    keys::parse_hex_key(std::addressof(nca.content_id), nca.name.c_str());
    log_write("generateing placeholder\n");
    R_TRY(ncmContentStorageGeneratePlaceHolderId(std::addressof(cs), std::addressof(nca.placeholder_id)));
    log_write("creating placeholder\n");
    R_TRY(ncmContentStorageCreatePlaceHolder(std::addressof(cs), std::addressof(nca.content_id), std::addressof(nca.placeholder_id), nca.size));

    log_write("opening thread\n");
    ThreadData t_data{this, tickets, std::addressof(nca)};

    #define READ_THREAD_CORE 1
    #define DECOMPRESS_THREAD_CORE 2
    #define WRITE_THREAD_CORE 0
    // #define READ_THREAD_CORE 2
    // #define DECOMPRESS_THREAD_CORE 2
    // #define WRITE_THREAD_CORE 2

    Thread t_read{};
    R_TRY(threadCreate(&t_read, readFunc, std::addressof(t_data), nullptr, 1024*64, 0x20, READ_THREAD_CORE));
    ON_SCOPE_EXIT(threadClose(&t_read));

    Thread t_decompress{};
    R_TRY(threadCreate(&t_decompress, decompressFunc, std::addressof(t_data), nullptr, 1024*64, 0x20, DECOMPRESS_THREAD_CORE));
    ON_SCOPE_EXIT(threadClose(&t_decompress));

    Thread t_write{};
    R_TRY(threadCreate(&t_write, writeFunc, std::addressof(t_data), nullptr, 1024*64, 0x20, WRITE_THREAD_CORE));
    ON_SCOPE_EXIT(threadClose(&t_write));

    log_write("starting threads\n");
    R_TRY(threadStart(std::addressof(t_read)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_read)));

    R_TRY(threadStart(std::addressof(t_decompress)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_decompress)));

    R_TRY(threadStart(std::addressof(t_write)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_write)));

    while (t_data.write_offset != t_data.write_size && R_SUCCEEDED(t_data.GetResults())) {
        pbox->UpdateTransfer(t_data.write_offset, t_data.write_size);
        svcSleepThread(1e+6);
    }

    // wait for all threads to close.
    log_write("waiting for threads to close\n");
    for (;;) {
        t_data.WakeAllThreads();
        pbox->Yield();

        if (R_FAILED(waitSingleHandle(t_read.handle, 1000))) {
            continue;
        } else if (R_FAILED(waitSingleHandle(t_decompress.handle, 1000))) {
            continue;
        } else if (R_FAILED(waitSingleHandle(t_write.handle, 1000))) {
            continue;
        }
        break;
    }
    log_write("threads closed\n");

    // if any of the threads failed, wake up all threads so they can exit.
    if (R_FAILED(t_data.GetResults())) {
        log_write("some reads failed, waking threads: %s\n", nca.name.c_str());
        log_write("returning due to fail: %s\n", nca.name.c_str());
        return t_data.GetResults();
    }
    R_TRY(t_data.GetResults());

    NcmContentId content_id{};
    std::memcpy(std::addressof(content_id), nca.hash, sizeof(content_id));

    log_write("old id: %s new id: %s\n", hexIdToStr(nca.content_id).str, hexIdToStr(content_id).str);
    log_write("doing register: %s\n", nca.name.c_str());
    if (!config.skip_nca_hash_verify && !nca.modified) {
        if (std::memcmp(&nca.content_id, nca.hash, sizeof(nca.content_id))) {
            log_write("nca hash is invalid!!!!\n");
            R_UNLESS(!std::memcmp(&nca.content_id, nca.hash, sizeof(nca.content_id)), Result_InvalidNcaSha256);
        } else {
            log_write("nca hash is valid!\n");
        }
    } else {
        log_write("skipping nca sha256 verify\n");
    }

    R_SUCCEED();
}

Result Yati::InstallCnmtNca(std::span<TikCollection> tickets, CnmtCollection& cnmt, const container::Collections& collections) {
    R_TRY(InstallNca(tickets, cnmt));

    fs::FsPath path;
    R_TRY(ncmContentStorageFlushPlaceHolder(std::addressof(cs)));
    R_TRY(ncmContentStorageGetPlaceHolderPath(std::addressof(cs), path, sizeof(path), std::addressof(cnmt.placeholder_id)));

    FsFileSystem fs;
    R_TRY(fsOpenFileSystem(std::addressof(fs), FsFileSystemType_ContentMeta, path));
    ON_SCOPE_EXIT(fsFsClose(std::addressof(fs)));

    FsDir dir;
    R_TRY(fsFsOpenDirectory(std::addressof(fs), fs::FsPath{"/"}, FsDirOpenMode_ReadFiles, std::addressof(dir)));
    ON_SCOPE_EXIT(fsDirClose(std::addressof(dir)));

    s64 total_entries;
    FsDirectoryEntry buf;
    R_TRY(fsDirRead(std::addressof(dir), std::addressof(total_entries), 1, std::addressof(buf)));

    FsFile file;
    R_TRY(fsFsOpenFile(std::addressof(fs), fs::AppendPath("/", buf.name), FsOpenMode_Read, std::addressof(file)));
    ON_SCOPE_EXIT(fsFileClose(std::addressof(file)));

    s64 offset{};
    u64 bytes_read;
    ncm::PackagedContentMeta header;
    R_TRY(fsFileRead(std::addressof(file), offset, std::addressof(header), sizeof(header), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    // read extended header
    cnmt.extended_header.resize(header.meta_header.extended_header_size);
    R_TRY(fsFileRead(std::addressof(file), offset, cnmt.extended_header.data(), cnmt.extended_header.size(), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    // read infos.
    std::vector<NcmPackagedContentInfo> infos(header.meta_header.content_count);
    R_TRY(fsFileRead(std::addressof(file), offset, infos.data(), infos.size() * sizeof(NcmPackagedContentInfo), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    for (const auto& info : infos) {
        if (info.info.content_type == NcmContentType_DeltaFragment) {
            continue;
        }

        const auto str = hexIdToStr(info.info.content_id);
        const auto it = std::find_if(collections.cbegin(), collections.cend(), [str](auto& e){
            return e.name.find(str.str) != e.name.npos;
        });

        R_UNLESS(it != collections.cend(), Result_NcaNotFound);

        log_write("found: %s\n", str.str);
        cnmt.infos.emplace_back(info);
        auto& nca = cnmt.ncas.emplace_back(*it);
        nca.type = info.info.content_type;
    }

    // update header
    cnmt.header = header.meta_header;
    cnmt.header.content_count = cnmt.infos.size() + 1;
    cnmt.header.storage_id = 0;

    cnmt.key.id = header.title_id;
    cnmt.key.version = header.title_version;
    cnmt.key.type = header.meta_type;
    cnmt.key.install_type = NcmContentInstallType_Full;
    std::memset(cnmt.key.padding, 0, sizeof(cnmt.key.padding));

    cnmt.content_info.content_id = cnmt.content_id;
    cnmt.content_info.content_type = NcmContentType_Meta;
    cnmt.content_info.attr = 0;
    ncmU64ToContentInfoSize(cnmt.size, &cnmt.content_info);
    cnmt.content_info.id_offset = 0;

    if (config.lower_system_version) {
        auto extended_header = (ncm::ExtendedHeader*)cnmt.extended_header.data();
        log_write("patching version\n");
        if (cnmt.key.type == NcmContentMetaType_Application) {
            extended_header->application.required_system_version = 0;
        } else if (cnmt.key.type == NcmContentMetaType_Patch) {
            extended_header->patch.required_system_version = 0;
        }
    }

    // sort ncas
    const auto sorter = [](NcaCollection& lhs, NcaCollection& rhs) -> bool {
        return lhs.type > rhs.type;
    };

    std::sort(cnmt.ncas.begin(), cnmt.ncas.end(), sorter);

    log_write("found all cnmts\n");
    R_SUCCEED();
}

Result Yati::InstallControlNca(std::span<TikCollection> tickets, const CnmtCollection& cnmt, NcaCollection& nca) {
    R_TRY(InstallNca(tickets, nca));

    fs::FsPath path;
    R_TRY(ncmContentStorageFlushPlaceHolder(std::addressof(cs)));
    R_TRY(ncmContentStorageGetPlaceHolderPath(std::addressof(cs), path, sizeof(path), std::addressof(nca.placeholder_id)));

    log_write("got control path: %s\n", path.s);
    FsFileSystem fs;
    R_TRY(fsOpenFileSystemWithId(std::addressof(fs), ncm::GetAppId(cnmt.key), FsFileSystemType_ContentControl, path, FsContentAttributes_All));
    ON_SCOPE_EXIT(fsFsClose(std::addressof(fs)));
    log_write("opened control path fs: %s\n", path.s);

    FsFile file;
    R_TRY(fsFsOpenFile(std::addressof(fs), fs::FsPath{"/control.nacp"}, FsOpenMode_Read, std::addressof(file)));
    ON_SCOPE_EXIT(fsFileClose(std::addressof(file)));
    log_write("got control path file: %s\n", path.s);

    NacpLanguageEntry entry;
    u64 bytes_read;
    R_TRY(fsFileRead(&file, 0, &entry, sizeof(entry), 0, &bytes_read));
    pbox->SetTitle("Installing "_i18n + entry.name);

    R_SUCCEED();
}

Result InstallInternal(ui::ProgressBox* pbox, std::shared_ptr<source::Base> source, const container::Collections& collections) {
    auto yati = std::make_unique<Yati>(pbox, source);
    R_TRY(yati->Setup());

    std::vector<TikCollection> tickets{};
    for (const auto& collection : collections) {
        if (collection.name.ends_with(".tik")) {
            TikCollection entry{};
            keys::parse_hex_key(entry.rights_id.c, collection.name.c_str());
            const auto str = collection.name.substr(0, collection.name.length() - 4) + ".cert";

            const auto cert = std::find_if(collections.cbegin(), collections.cend(), [str](auto& e){
                return e.name.find(str) != e.name.npos;
            });

            R_UNLESS(cert != collections.cend(), Result_CertNotFound);
            entry.ticket.resize(collection.size);
            entry.cert.resize(cert->size);

            u64 bytes_read;
            R_TRY(source->Read(entry.ticket.data(), collection.offset, entry.ticket.size(), &bytes_read));
            R_TRY(source->Read(entry.cert.data(), cert->offset, entry.cert.size(), &bytes_read));
            tickets.emplace_back(entry);
        }
    }

    std::vector<CnmtCollection> cnmts{};
    for (const auto& collection : collections) {
        log_write("found collection: %s\n", collection.name.c_str());
        if (collection.name.ends_with(".cnmt.nca")) {
            auto& cnmt = cnmts.emplace_back(NcaCollection{collection});
            cnmt.type = NcmContentType_Meta;
        }
    }

    for (auto& cnmt : cnmts) {
        ON_SCOPE_EXIT(
            ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(cnmt.placeholder_id));
            for (auto& nca : cnmt.ncas) {
                ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(nca.placeholder_id));
            }
        );

        R_TRY(yati->InstallCnmtNca(tickets, cnmt, collections));

        bool skip = false;
        const auto app_id = ncm::GetAppId(cnmt.key);
        bool has_records;
        R_TRY(nsIsAnyApplicationEntityInstalled(app_id, &has_records));

        // TODO: fix this when gamecard is inserted as it will only return records
        // for the gamecard...
        // may have to use ncm directly to get the keys, then parse that.
        u32 latest_version_num = cnmt.key.version;
        if (has_records) {
            s32 meta_count{};
            R_TRY(nsCountApplicationContentMeta(app_id, &meta_count));
            R_UNLESS(meta_count > 0, 0x1);

            std::vector<ncm::ContentStorageRecord> records(meta_count);
            s32 count;
            R_TRY(ns::ListApplicationRecordContentMeta(std::addressof(yati->ns_app), 0, app_id, records.data(), records.size(), &count));
            R_UNLESS(count == records.size(), 0x1);

            for (auto& record : records) {
                log_write("found record: 0x%016lX type: %u version: %u\n", record.key.id, record.key.type, record.key.version);
                log_write("cnmt record: 0x%016lX type: %u version: %u\n", cnmt.key.id, cnmt.key.type, cnmt.key.version);

                if (record.key.id == cnmt.key.id && cnmt.key.version == record.key.version && yati->config.skip_if_already_installed) {
                    log_write("skipping as already installed\n");
                    skip = true;
                }

                // check if we are downgrading
                if (cnmt.key.type == NcmContentMetaType_Patch) {
                    if (cnmt.key.type == record.key.type && cnmt.key.version < record.key.version && !yati->config.allow_downgrade) {
                        log_write("skipping due to it being lower\n");
                        skip = true;
                    }
                } else {
                    latest_version_num = std::max(latest_version_num, record.key.version);
                }
            }
        }

        // skip invalid types
        if (!(cnmt.key.type & 0x80)) {
            log_write("\tskipping: invalid: %u\n", cnmt.key.type);
            skip = true;
        } else if (yati->config.skip_base && cnmt.key.type == NcmContentMetaType_Application) {
            log_write("\tskipping: [NcmContentMetaType_Application]\n");
            skip = true;
        } else if (yati->config.skip_patch && cnmt.key.type == NcmContentMetaType_Patch) {
            log_write("\tskipping: [NcmContentMetaType_Application]\n");
            skip = true;
        } else if (yati->config.skip_addon && cnmt.key.type == NcmContentMetaType_AddOnContent) {
            log_write("\tskipping: [NcmContentMetaType_AddOnContent]\n");
            skip = true;
        } else if (yati->config.skip_data_patch && cnmt.key.type == NcmContentMetaType_DataPatch) {
            log_write("\tskipping: [NcmContentMetaType_DataPatch]\n");
            skip = true;
        }

        if (skip) {
            log_write("skipping install!\n");
            continue;
        }

        log_write("installing nca's\n");
        for (auto& nca : cnmt.ncas) {
            if (nca.type == NcmContentType_Control) {
                log_write("installing control nca\n");
                R_TRY(yati->InstallControlNca(tickets, cnmt, nca));
            } else {
                R_TRY(yati->InstallNca(tickets, nca));
            }
        }

        // log_write("exiting early :)\n");
        // return 0;

        for (auto& ticket : tickets) {
            if (ticket.required) {
                if (yati->config.skip_ticket) {
                    log_write("WARNING: skipping ticket install, but it's required!\n");
                } else {
                    log_write("patching ticket\n");
                    if (yati->config.patch_ticket) {
                        R_TRY(es::PatchTicket(ticket.ticket, yati->keys, false));
                    }
                    log_write("installing ticket\n");
                    R_TRY(es::ImportTicket(std::addressof(yati->es), ticket.ticket.data(), ticket.ticket.size(), ticket.cert.data(), ticket.cert.size()));
                    ticket.required = false;
                }
            }
        }

        log_write("listing keys\n");

        // remove current entries (if any).
        s32 db_list_total;
        s32 db_list_count;
        u64 id_min = cnmt.key.id;
        u64 id_max = cnmt.key.id;
        std::vector<NcmContentMetaKey> keys(1);

        // if installing a patch, remove all previously installed patches.
        if (cnmt.key.type == NcmContentMetaType_Patch) {
            id_min = 0;
            id_max = UINT64_MAX;
        }

        for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
            auto& cs = yati->ncm_cs[i];
            auto& db = yati->ncm_db[i];

            std::vector<NcmContentMetaKey> keys(1);
            R_TRY(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), static_cast<NcmContentMetaType>(cnmt.key.type), app_id, id_min, id_max, NcmContentInstallType_Full));

            if (db_list_total != keys.size()) {
                keys.resize(db_list_total);
                if (keys.size()) {
                    R_TRY(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), static_cast<NcmContentMetaType>(cnmt.key.type), app_id, id_min, id_max, NcmContentInstallType_Full));
                }
            }

            for (auto& key : keys) {
                log_write("found key: 0x%016lX type: %u version: %u\n", key.id, key.type, key.version);
                NcmContentMetaHeader header;
                u64 out_size;
                log_write("trying to get from db\n");
                R_TRY(ncmContentMetaDatabaseGet(std::addressof(db), std::addressof(key), std::addressof(out_size), std::addressof(header), sizeof(header)));
                R_UNLESS(out_size == sizeof(header), Result_NcmDbCorruptHeader);
                log_write("trying to list infos\n");

                std::vector<NcmContentInfo> infos(header.content_count);
                s32 content_info_out;
                R_TRY(ncmContentMetaDatabaseListContentInfo(std::addressof(db), std::addressof(content_info_out), infos.data(), infos.size(), std::addressof(key), 0));
                R_UNLESS(content_info_out == infos.size(), Result_NcmDbCorruptInfos);
                log_write("size matches\n");

                for (auto& info : infos) {
                    R_TRY(ncm::Delete(std::addressof(cs), std::addressof(info.content_id)));
                }

                log_write("trying to remove it\n");
                R_TRY(ncmContentMetaDatabaseRemove(std::addressof(db), std::addressof(key)));
                R_TRY(ncmContentMetaDatabaseCommit(std::addressof(db)));
                log_write("all done with this key\n\n");
            }
        }

        log_write("done with keys\n");

        // register all nca's
        log_write("registering cnmt nca\n");
        R_TRY(ncm::Register(std::addressof(yati->cs), std::addressof(cnmt.content_id), std::addressof(cnmt.placeholder_id)));
        log_write("registered cnmt nca\n");

        for (auto& nca : cnmt.ncas) {
            log_write("registering nca: %s\n", nca.name.c_str());
            R_TRY(ncm::Register(std::addressof(yati->cs), std::addressof(nca.content_id), std::addressof(nca.placeholder_id)));
            log_write("registered nca: %s\n", nca.name.c_str());
        }

        log_write("register'd all ncas\n");

        {
            BufHelper buf{};
            buf.write(std::addressof(cnmt.header), sizeof(cnmt.header));
            buf.write(cnmt.extended_header.data(), cnmt.extended_header.size());
            buf.write(std::addressof(cnmt.content_info), sizeof(cnmt.content_info));

            for (auto& info : cnmt.infos) {
                buf.write(std::addressof(info.info), sizeof(info.info));
            }

            pbox->NewTransfer("Updating ncm databse"_i18n);
            R_TRY(ncmContentMetaDatabaseSet(std::addressof(yati->db), std::addressof(cnmt.key), buf.buf.data(), buf.tell()));
            R_TRY(ncmContentMetaDatabaseCommit(std::addressof(yati->db)));
        }

        {
            ncm::ContentStorageRecord content_storage_record{};
            content_storage_record.key = cnmt.key;
            content_storage_record.storage_id = yati->storage_id;
            pbox->NewTransfer("Pushing application record"_i18n);

            R_TRY(ns::PushApplicationRecord(std::addressof(yati->ns_app), app_id, std::addressof(content_storage_record), 1));
            if (hosversionAtLeast(6,0,0)) {
                R_TRY(avmInitialize());
                ON_SCOPE_EXIT(avmExit());

                R_TRY(avmPushLaunchVersion(app_id, latest_version_num));
            }
            log_write("pushed\n");
        }
    }

    log_write("success!\n");
    R_SUCCEED();
}

} // namespace

Result InstallFromFile(ui::ProgressBox* pbox, FsFileSystem* fs, const fs::FsPath& path) {
    return InstallFromSource(pbox, std::make_shared<source::File>(fs, path), path);
}

Result InstallFromStdioFile(ui::ProgressBox* pbox, const fs::FsPath& path) {
    return InstallFromSource(pbox, std::make_shared<source::Stdio>(path), path);
}

Result InstallFromSource(ui::ProgressBox* pbox, std::shared_ptr<source::Base> source, const fs::FsPath& path) {
    if (R_SUCCEEDED(container::Nsp::Validate(source.get()))) {
        log_write("found nsp\n");
        return InstallFromContainer(pbox, std::make_unique<container::Nsp>(source));
    } else if (R_SUCCEEDED(container::Xci::Validate(source.get()))) {
        log_write("found xci\n");
        return InstallFromContainer(pbox, std::make_unique<container::Xci>(source));
    } else {
        log_write("found unknown container\n");
        R_THROW(Result_ContainerNotFound);
    }
}

Result InstallFromContainer(ui::ProgressBox* pbox, std::shared_ptr<container::Base> container) {
    container::Collections collections;
    R_TRY(container->GetCollections(collections));
    return InstallFromCollections(pbox, container->GetSource(), collections);
}

Result InstallFromCollections(ui::ProgressBox* pbox, std::shared_ptr<source::Base> source, const container::Collections& collections) {
    return InstallInternal(pbox, source, collections);
}

} // namespace sphaira::yati
