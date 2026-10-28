#pragma once
// ---------------------------------------------------------------------------
// TS 側に伝送された 4K データ放送 (ARIB STD-B62) の受信
//
// nanamitm/dantto4k が MMT/TLV -> MPEG-2 TS 変換時に、データ放送を出力 TS の
// 専用 PID (stream_type 0x05, registration_descriptor format_identifier
// 'D4KA') に private_section として載せる方式 (docs/app-data-ts-transport.md)
// の「受信側」。TVTest のストリームコールバックで受け取る全 TS パケットから
// この PID を拾い、MH-AIT / DDMT / DAMT とアイテム (0xC0) を組み立てて、
// 既存の DB4K 方式と同じ getApplicationInfo / getFileByPath を提供する。
//
// DB4K_* (GetModuleHandle 経由の自作 dantto フォーク) が無くても、nanamitm の
// dantto を BonDriver に使えばデータ放送を表示できるようにするためのもの。
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <optional>
#include <tuple>
#include <memory>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <windows.h>

namespace AppDataTs {

// ---- 最小 ReadStream (ビッグエンディアン、境界超過で例外) ----
class ReadStream {
public:
    ReadStream(const uint8_t* p, size_t n) : p_(p), n_(n), pos_(0) {}
    uint8_t get8U() { need(1); return p_[pos_++]; }
    uint16_t getBe16U() { need(2); uint16_t v = (p_[pos_] << 8) | p_[pos_ + 1]; pos_ += 2; return v; }
    uint32_t getBe32U() { need(4); uint32_t v = ((uint32_t)p_[pos_] << 24) | ((uint32_t)p_[pos_ + 1] << 16) | ((uint32_t)p_[pos_ + 2] << 8) | p_[pos_ + 3]; pos_ += 4; return v; }
    uint16_t peekBe16U() const { if (pos_ + 2 > n_) throw std::out_of_range("peek"); return (p_[pos_] << 8) | p_[pos_ + 1]; }
    void read(char* dst, size_t n) { need(n); memcpy(dst, p_ + pos_, n); pos_ += n; }
    void read(uint8_t* dst, size_t n) { need(n); memcpy(dst, p_ + pos_, n); pos_ += n; }
    void skip(size_t n) { need(n); pos_ += n; }
    size_t leftBytes() const { return n_ - pos_; }
    bool isEof() const { return pos_ >= n_; }
    const uint8_t* cur() const { return p_ + pos_; }
private:
    void need(size_t n) const { if (pos_ + n > n_) throw std::out_of_range("read"); }
    const uint8_t* p_; size_t n_; size_t pos_;
};

// ---- zlib1.dll を動的に使う inflate ----
class Zlib {
public:
    static bool decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t knownSize) {
        auto& z = instance();
        if (!z.ok_) return false;
        if (knownSize > 0 && z.uncompress_) {
            out.resize(knownSize);
            unsigned long destLen = (unsigned long)knownSize;
            int r = z.uncompress_(out.data(), &destLen, in, (unsigned long)inLen);
            if (r == 0) { out.resize(destLen); return true; }
            // 失敗したらストリーミングにフォールバック
        }
        return z.inflateStream(in, inLen, out);
    }
private:
    // zlib の最小型 (z_stream の先頭レイアウトのみ)
    struct zstream {
        const uint8_t* next_in; unsigned int avail_in; unsigned long total_in;
        uint8_t* next_out; unsigned int avail_out; unsigned long total_out;
        const char* msg; void* state;
        void* zalloc; void* zfree; void* opaque;
        int data_type; unsigned long adler; unsigned long reserved;
    };
    typedef int(__cdecl* uncompress_t)(uint8_t*, unsigned long*, const uint8_t*, unsigned long);
    typedef int(__cdecl* inflateInit2_t)(zstream*, int, const char*, int);
    typedef int(__cdecl* inflate_t)(zstream*, int);
    typedef int(__cdecl* inflateEnd_t)(zstream*);

    HMODULE dll_ = nullptr; bool ok_ = false;
    uncompress_t uncompress_ = nullptr;
    inflateInit2_t inflateInit2_ = nullptr;
    inflate_t inflate_ = nullptr;
    inflateEnd_t inflateEnd_ = nullptr;

    Zlib() {
        dll_ = LoadLibraryW(L"zlib1.dll");
        if (!dll_) dll_ = LoadLibraryW(L"zlibwapi.dll");
        if (!dll_) return;
        uncompress_ = (uncompress_t)GetProcAddress(dll_, "uncompress");
        inflateInit2_ = (inflateInit2_t)GetProcAddress(dll_, "inflateInit2_");
        inflate_ = (inflate_t)GetProcAddress(dll_, "inflate");
        inflateEnd_ = (inflateEnd_t)GetProcAddress(dll_, "inflateEnd");
        ok_ = inflateInit2_ && inflate_ && inflateEnd_;
    }
    static Zlib& instance() { static Zlib z; return z; }

    bool inflateStream(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
        zstream s; memset(&s, 0, sizeof(s));
        // windowBits=47: zlib/gzip 自動判別
        if (inflateInit2_(&s, 47, "1.2.11", (int)sizeof(zstream)) != 0) return false;
        out.clear(); out.resize(inLen * 4 + 4096);
        s.next_in = in; s.avail_in = (unsigned int)inLen;
        s.next_out = out.data(); s.avail_out = (unsigned int)out.size();
        int r;
        for (;;) {
            r = inflate_(&s, 0); // Z_NO_FLUSH
            if (r == 1) break;   // Z_STREAM_END
            if (r != 0) { inflateEnd_(&s); return false; }
            if (s.avail_out == 0) {
                size_t used = out.size();
                out.resize(out.size() * 2);
                s.next_out = out.data() + used;
                s.avail_out = (unsigned int)(out.size() - used);
            } else if (s.avail_in == 0) {
                break;
            }
        }
        out.resize(s.total_out);
        inflateEnd_(&s);
        return true;
    }
};

// ---------------------------------------------------------------------------
class Receiver {
public:
    struct AppInfo {
        uint16_t applicationType = 0;
        uint32_t organizationId = 0;
        uint16_t applicationId = 0;
        uint8_t controlCode = 0;
        std::string entryUrl;
        bool valid = false;
    };

    // TVTest のストリームコールバック (別スレッド) から 1 パケットずつ渡す。
    void processPacket(const uint8_t* ts) {
        if (ts[0] != 0x47) return;
        uint16_t pid = ((ts[1] & 0x1F) << 8) | ts[2];
        bool payloadStart = (ts[1] & 0x40) != 0;
        uint8_t afc = (ts[3] >> 4) & 0x3;
        size_t off = 4;
        if (afc == 0 || afc == 2) return; // ペイロード無し
        if (afc == 3) { if (off >= 188) return; off += 1 + ts[4]; }
        if (off >= 188) return;
        const uint8_t* payload = ts + off;
        size_t payloadLen = 188 - off;

        std::lock_guard<std::mutex> lock(mutex_);
        if (pid == 0) { feedSection(pat_, payload, payloadLen, payloadStart, [this](const uint8_t* d, size_t n) { parsePat(d, n); }); return; }
        if (pmtPids_.count(pid)) { feedSection(sections_[pid], payload, payloadLen, payloadStart, [this](const uint8_t* d, size_t n) { parsePmt(d, n); }); return; }
        if (d4kaPid_ && pid == *d4kaPid_) {
            feedSection(sections_[pid], payload, payloadLen, payloadStart, [this](const uint8_t* d, size_t n) { parseD4kaSection(d, n); });
        }
    }

    bool getApplicationInfo(AppInfo& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ait_.valid) return false;
        out = ait_;
        return true;
    }

    bool getFileByPath(const std::string& path, std::vector<uint8_t>& data, std::string& contentType) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string n = normalize(path);
        auto it = files_.find(n);
        if (it == files_.end()) {
            std::string base = normalize(basePath_);
            if (!base.empty() && n.rfind(base, 0) == 0) {
                it = files_.find(normalize(n.substr(base.size())));
            }
        }
        if (it == files_.end()) return false;
        data = *it->second.data;
        contentType = it->second.contentType;
        return true;
    }

    bool hasData() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return d4kaPid_.has_value() && ait_.valid;
    }

    // D4KA PID が見つかっているか (MH-AIT 未受信でも true)。DB4K とどちらを使うか判定用。
    bool hasPid() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return d4kaPid_.has_value();
    }

    std::vector<std::string> listFiles() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> r;
        for (auto& [k, v] : files_) r.push_back(k + "\t" + v.contentType + "\t" + std::to_string(v.data->size()));
        std::sort(r.begin(), r.end());
        return r;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pat_.clear(); sections_.clear(); pmtPids_.clear(); d4kaPid_.reset();
        ait_ = {}; basePath_.clear(); nodeTagToPath_.clear(); dirNodeTagToPath_.clear();
        damtMpus_.clear(); items_.clear(); indexes_.clear(); files_.clear(); itemAssembly_.clear();
    }

private:
    // ========================= TS / セクション =========================
    struct SectionBuf {
        std::vector<uint8_t> data;
        size_t expected = 0; // table 全体長 (3 + section_length)
        bool active = false;
        void clear() { data.clear(); expected = 0; active = false; }
    };

    template <class Fn>
    void feedSection(SectionBuf& buf, const uint8_t* payload, size_t len, bool start, Fn&& onSection) {
        size_t i = 0;
        if (start) {
            if (len < 1) return;
            uint8_t pointer = payload[0];
            i = 1 + pointer;
            if (i > len) return;
            buf.clear();
            buf.active = true;
        }
        if (!buf.active) return;
        while (i < len) {
            if (buf.expected == 0) {
                // 新しいセクションの先頭
                if (payload[i] == 0xFF) { i = len; break; } // スタッフィング
                // table_id(1) + section_length(byte1-2 の下位12bit) を見るのに3バイト必要
                // 足りなければ buf に貯めてから判定
                buf.data.push_back(payload[i++]);
                if (buf.data.size() < 3) continue;
                uint16_t sl = ((buf.data[1] & 0x0F) << 8) | buf.data[2];
                buf.expected = 3 + sl;
            } else {
                size_t need = buf.expected - buf.data.size();
                size_t avail = len - i;
                size_t take = need < avail ? need : avail;
                buf.data.insert(buf.data.end(), payload + i, payload + i + take);
                i += take;
            }
            if (buf.expected != 0 && buf.data.size() >= buf.expected) {
                try { onSection(buf.data.data(), buf.expected); } catch (...) {}
                // 1 パケットに複数セクションが続く場合に備えてリセット
                buf.data.clear(); buf.expected = 0;
            }
        }
    }

    void parsePat(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            uint8_t tid = s.get8U();
            if (tid != 0x00) return;
            uint16_t sl = s.getBe16U() & 0x0FFF;
            s.skip(5); // tsid(2)+ver/cur(1)+sec(1)+last(1)
            size_t programBytes = (sl >= 5 + 4) ? (sl - 5 - 4) : 0; // 残り - CRC
            std::unordered_map<uint16_t, bool> newPmt;
            for (size_t b = 0; b + 4 <= programBytes; b += 4) {
                uint16_t prog = s.getBe16U();
                uint16_t pid = s.getBe16U() & 0x1FFF;
                if (prog != 0) newPmt[pid] = true;
            }
            // PMT PID 集合を更新
            for (auto& [pid, _] : newPmt) {
                if (!pmtPids_.count(pid)) pmtPids_[pid] = true;
            }
        } catch (...) {}
    }

    void parsePmt(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            uint8_t tid = s.get8U();
            if (tid != 0x02) return;
            uint16_t sl = s.getBe16U() & 0x0FFF;
            s.getBe16U(); // program_number
            s.skip(3); // ver/cur, sec, last
            s.getBe16U(); // PCR_PID
            uint16_t programInfoLength = s.getBe16U() & 0x0FFF;
            s.skip(programInfoLength);
            // ES ループ: section_length から CRC(4) と ここまでを引いた残り
            // 残りバイトを leftBytes()-4(CRC) で回す
            while (s.leftBytes() > 4) {
                uint8_t streamType = s.get8U();
                uint16_t esPid = s.getBe16U() & 0x1FFF;
                uint16_t esInfoLen = s.getBe16U() & 0x0FFF;
                const uint8_t* esInfo = s.cur();
                // registration_descriptor (0x05) で 'D4KA' を探す
                if (streamType == 0x05) {
                    ReadStream ds(esInfo, esInfoLen);
                    while (ds.leftBytes() >= 2) {
                        uint8_t dtag = ds.get8U();
                        uint8_t dlen = ds.get8U();
                        if (ds.leftBytes() < dlen) break;
                        if (dtag == 0x05 && dlen >= 4) {
                            uint32_t fmt = (ds.cur()[0] << 24) | (ds.cur()[1] << 16) | (ds.cur()[2] << 8) | ds.cur()[3];
                            if (fmt == 0x44344B41 /*'D4KA'*/) {
                                if (!d4kaPid_ || *d4kaPid_ != esPid) {
                                    d4kaPid_ = esPid;
                                    sections_[esPid].clear();
                                }
                            }
                        }
                        ds.skip(dlen);
                    }
                }
                s.skip(esInfoLen);
            }
        } catch (...) {}
    }

    void parseD4kaSection(const uint8_t* d, size_t n) {
        if (n < 1) return;
        uint8_t tableId = d[0];
        switch (tableId) {
        case 0x9C: parseMhAit(d, n); break;       // MH-AIT
        case 0xA3: parseDdmt(d, n); break;        // DDMT
        case 0xA4: parseDamt(d, n); break;        // DAMT
        case 0xC0: parseItemSection(d, n); break; // アイテム (dantto 独自)
        // 0xA5 DCCT / 0xC1 字幕 / 0xC2 EMT は現状未使用
        default: break;
        }
    }

    // ========================= テーブルパーサ =========================
    void parseMhAit(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            s.get8U(); // table_id
            uint16_t sl = s.getBe16U() & 0x0FFF; (void)sl;
            uint16_t applicationType = s.getBe16U();
            s.get8U(); // ver/cur
            s.get8U(); s.get8U(); // sec/last
            uint16_t commonDescLen = s.getBe16U() & 0x0FFF;
            s.skip(commonDescLen);
            uint16_t appLoopLen = s.getBe16U() & 0x0FFF;
            ReadStream as(s.cur(), appLoopLen);

            AppInfo info{}; info.applicationType = applicationType;
            while (as.leftBytes() > 0) {
                // ApplicationIdentifier (nekohkr 準拠: org=Be16, app=Be32)
                uint32_t org = as.getBe16U();
                uint16_t app = (uint16_t)as.getBe32U();
                uint8_t ctrl = as.get8U();
                uint16_t descLoopLen = as.getBe16U() & 0x0FFF;
                ReadStream ds(as.cur(), descLoopLen);
                std::string urlBase, location;
                while (ds.leftBytes() >= 3) {
                    uint16_t dtag = ds.getBe16U();
                    uint8_t dlen = ds.get8U();
                    if (ds.leftBytes() < dlen) break;
                    const uint8_t* dp = ds.cur();
                    if (dtag == 0x802B) { // simple_application_location
                        location.assign((const char*)dp, dlen);
                    } else if (dtag == 0x802A && dlen >= 3) { // transport_protocol
                        uint16_t protocolId = (dp[0] << 8) | dp[1];
                        if (protocolId == 0x0003) {
                            // selector = dp[3..], 先頭 1 バイト長 + URL_base
                            if (dlen >= 4) {
                                uint8_t blen = dp[3];
                                if (4 + blen <= dlen) urlBase.assign((const char*)dp + 4, blen);
                            }
                        }
                    }
                    ds.skip(dlen);
                }
                if (!location.empty()) {
                    info.organizationId = org; info.applicationId = app; info.controlCode = ctrl;
                    info.entryUrl = urlBase.empty() ? location : urlBase + location;
                    info.valid = true;
                    if (ctrl == 0x01) break; // AUTOSTART 優先
                }
            }
            ait_ = info;
        } catch (...) {}
    }

    void parseDdmt(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            s.get8U(); // table_id
            s.getBe16U(); // section_syntax + length
            s.get8U(); // dataTransmissionSessionId
            s.skip(1); // reserved
            s.get8U(); // ver/cur
            s.get8U(); s.get8U(); // sec/last
            uint8_t baseLen = s.get8U();
            std::string base(baseLen, '\0'); s.read(base.data(), baseLen);
            basePath_ = base;
            uint8_t numNodes = s.get8U();
            for (uint8_t i = 0; i < numNodes; i++) {
                uint16_t nodeTag = s.getBe16U();
                s.get8U(); // directoryNodeVersion
                uint8_t pathLen = s.get8U();
                std::string dir(pathLen, '\0'); s.read(dir.data(), pathLen);
                if (!dir.empty() && dir.back() != '/') dir += '/';
                dirNodeTagToPath_[nodeTag] = dir;
                uint16_t numFiles = s.getBe16U();
                for (uint16_t f = 0; f < numFiles; f++) {
                    uint16_t fileTag = s.getBe16U();
                    uint8_t fnLen = s.get8U();
                    std::string fn(fnLen, '\0'); s.read(fn.data(), fnLen);
                    nodeTagToPath_[fileTag] = dir + fn;
                }
            }
            rebuildFiles();
        } catch (...) {}
    }

    void parseDamt(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            s.get8U(); // table_id
            s.getBe16U(); // section_syntax + length
            s.get8U(); // dataTransmissionSessionId
            s.skip(1); // reserved
            s.get8U(); // ver/cur
            s.get8U(); s.get8U(); // sec/last
            s.getBe32U(); // transactionId
            uint16_t componentTag = s.getBe16U();
            s.getBe32U(); // downloadId
            uint8_t numMpus = s.get8U();
            for (uint8_t i = 0; i < numMpus; i++) {
                uint32_t mpuSeq = s.getBe32U();
                s.getBe32U(); // mpuSize
                uint8_t flags = s.get8U();
                bool indexItemFlag = (flags & 0x80) >> 7;
                bool indexItemIdFlag = (flags & 0x40) >> 6;
                uint8_t compression = (flags & 0x30) >> 4;
                uint32_t indexItemId = 0;
                if (indexItemFlag && indexItemIdFlag) indexItemId = s.getBe32U();
                uint16_t numItems = s.getBe16U();
                DamtMpu m; m.indexItemFlag = indexItemFlag; m.indexItemId = indexItemId; m.indexItemCompression = compression;
                for (uint16_t it = 0; it < numItems; it++) {
                    uint16_t nodeTag = s.getBe16U();
                    if (!indexItemFlag) {
                        uint32_t itemId = s.getBe32U();
                        s.getBe32U(); // itemSize
                        s.get8U(); // itemVersion
                        uint8_t cf = s.get8U();
                        if (cf & 0x80) s.getBe32U(); // checksum
                        uint8_t infoLen = s.get8U();
                        s.skip(infoLen);
                        m.nodeTags.push_back(nodeTag);
                        m.itemIds.push_back(itemId);
                    }
                }
                uint8_t mpuInfoLen = s.get8U();
                if (mpuInfoLen == 0x5) { s.skip(3); m.directoryNodeTag = s.getBe16U(); }
                else s.skip(mpuInfoLen);
                damtMpus_[MpuKey{ componentTag, mpuSeq }] = std::move(m);
            }
            rebuildFiles();
        } catch (...) {}
    }

    // nanamitm 独自アイテムセクション (0xC0)
    void parseItemSection(const uint8_t* d, size_t n) {
        try {
            ReadStream s(d, n);
            s.get8U(); // table_id
            s.getBe16U(); // section_syntax + length
            uint16_t sequence = s.getBe16U(); // table_id_extension = 送信通番下位16bit
            s.get8U(); // ver/cur
            uint8_t sectionNumber = s.get8U();
            uint8_t lastSectionNumber = s.get8U();
            // payload は末尾 CRC(4) を除く
            size_t bodyLen = (n >= 4) ? (n - 4) : 0;
            size_t headerConsumed = 8; // table_id..last_section_number
            if (bodyLen < headerConsumed) return;

            ItemAssembly& a = itemAssembly_[sequence];
            if (sectionNumber == 0) {
                // 先頭セクションにアイテムヘッダ
                a = ItemAssembly{};
                a.componentTag = s.getBe16U();
                a.mpuSeq = s.getBe32U();
                a.itemId = s.getBe32U();
                a.itemSize = s.getBe32U();
                a.lastSection = lastSectionNumber;
                a.hasHeader = true;
                headerConsumed += 14;
            }
            if (!a.hasHeader) return; // 先頭未受信
            a.sections[sectionNumber].assign(d + headerConsumed, d + bodyLen);
            // 全セクション揃ったか
            if (a.sections.size() == (size_t)a.lastSection + 1) {
                std::vector<uint8_t> data;
                for (uint8_t i = 0; i <= a.lastSection; i++) {
                    auto it = a.sections.find(i);
                    if (it == a.sections.end()) return;
                    data.insert(data.end(), it->second.begin(), it->second.end());
                }
                ItemKey key{ a.componentTag, a.mpuSeq, a.itemId };
                onItemComplete(key, std::move(data));
                itemAssembly_.erase(sequence);
            }
        } catch (...) {}
    }

    // ========================= ファイル構築 (dataBroadcastHandler 移植) =========================
    struct IndexEntry { uint32_t itemId; uint32_t itemSize; uint8_t itemVersion; std::string fileName; std::string itemType; uint8_t compressionType; uint32_t originalSize; };
    struct ItemKey { uint16_t componentTag; uint32_t mpuSequenceNumber; uint32_t itemId; bool operator<(const ItemKey& o) const { return std::tie(componentTag, mpuSequenceNumber, itemId) < std::tie(o.componentTag, o.mpuSequenceNumber, o.itemId); } };
    struct MpuKey { uint16_t componentTag; uint32_t mpuSequenceNumber; bool operator<(const MpuKey& o) const { return std::tie(componentTag, mpuSequenceNumber) < std::tie(o.componentTag, o.mpuSequenceNumber); } };
    struct DamtMpu { uint8_t indexItemCompression = 3; uint32_t indexItemId = 0; bool indexItemFlag = false; std::vector<uint16_t> nodeTags; std::vector<uint32_t> itemIds; std::optional<uint16_t> directoryNodeTag; };
    struct File { std::shared_ptr<const std::vector<uint8_t>> data; std::string contentType; };
    struct ItemAssembly { uint16_t componentTag = 0; uint32_t mpuSeq = 0; uint32_t itemId = 0; uint32_t itemSize = 0; uint8_t lastSection = 0; bool hasHeader = false; std::map<uint8_t, std::vector<uint8_t>> sections; };

    void onItemComplete(const ItemKey& key, std::vector<uint8_t>&& data) {
        MpuKey mkey{ key.componentTag, key.mpuSequenceNumber };
        auto damtIt = damtMpus_.find(mkey);
        bool isIndex = (damtIt != damtMpus_.end()) ? (damtIt->second.indexItemFlag && key.itemId == damtIt->second.indexItemId) : (key.itemId == 0);
        if (isIndex) {
            std::vector<uint8_t> raw = std::move(data);
            uint8_t compression = damtIt != damtMpus_.end() ? damtIt->second.indexItemCompression : 3;
            if (compression != 3) { std::vector<uint8_t> out; if (Zlib::decompress(raw.data(), raw.size(), out, 0)) raw = std::move(out); }
            std::vector<IndexEntry> entries;
            size_t p = 0;
            auto u8 = [&](uint8_t& v) { if (p + 1 > raw.size()) return false; v = raw[p++]; return true; };
            auto u32 = [&](uint32_t& v) { if (p + 4 > raw.size()) return false; v = (raw[p] << 24) | (raw[p + 1] << 16) | (raw[p + 2] << 8) | raw[p + 3]; p += 4; return true; };
            auto str = [&](std::string& ss) { uint8_t len; if (!u8(len) || p + len > raw.size()) return false; ss.assign((const char*)raw.data() + p, len); p += len; return true; };
            if (raw.size() >= 2) {
                uint16_t cnt = (raw[0] << 8) | raw[1]; p = 2;
                for (uint16_t i = 0; i < cnt; i++) {
                    IndexEntry e{}; uint8_t flags;
                    if (!u32(e.itemId) || !u32(e.itemSize) || !u8(e.itemVersion) || !str(e.fileName) || !u8(flags)) break;
                    if (flags & 0x80) { uint32_t cs; if (!u32(cs)) break; }
                    if (!str(e.itemType) || !u8(e.compressionType)) break;
                    if (e.compressionType != 0xFF) { if (!u32(e.originalSize)) break; }
                    entries.push_back(std::move(e));
                }
            }
            indexes_[mkey] = std::move(entries);
        } else {
            items_[key] = std::move(data);
        }
        rebuildFiles();
    }

    void rebuildFiles() {
        files_.clear();
        for (auto& [mkey, m] : damtMpus_) {
            if (m.indexItemFlag) continue;
            for (size_t k = 0; k < m.nodeTags.size() && k < m.itemIds.size(); k++) {
                auto itemIt = items_.find(ItemKey{ mkey.componentTag, mkey.mpuSequenceNumber, m.itemIds[k] });
                auto p = nodeTagToPath_.find(m.nodeTags[k]);
                if (itemIt == items_.end() || p == nodeTagToPath_.end()) continue;
                files_[normalize(p->second)] = File{ std::make_shared<const std::vector<uint8_t>>(itemIt->second), "" };
            }
        }
        for (auto& [mkey, entries] : indexes_) {
            auto damtIt = damtMpus_.find(mkey);
            for (size_t k = 0; k < entries.size(); k++) {
                auto& e = entries[k];
                auto itemIt = items_.find(ItemKey{ mkey.componentTag, mkey.mpuSequenceNumber, e.itemId });
                if (itemIt == items_.end()) continue;
                std::vector<std::string> paths;
                if (damtIt != damtMpus_.end()) {
                    auto& m = damtIt->second;
                    if (k < m.nodeTags.size()) {
                        auto p = nodeTagToPath_.find(m.nodeTags[k]);
                        if (p != nodeTagToPath_.end()) paths.push_back(p->second);
                        auto dnode = dirNodeTagToPath_.find(m.nodeTags[k]);
                        if (dnode != dirNodeTagToPath_.end()) paths.push_back(dnode->second + e.fileName);
                    }
                    if (m.directoryNodeTag) {
                        auto dnode = dirNodeTagToPath_.find(*m.directoryNodeTag);
                        if (dnode != dirNodeTagToPath_.end()) paths.push_back(dnode->second + e.fileName);
                    }
                }
                for (auto& [tag, path] : nodeTagToPath_) {
                    auto slash = path.find_last_of('/');
                    if ((slash == std::string::npos ? path : path.substr(slash + 1)) == e.fileName) paths.push_back(path);
                }
                paths.push_back(e.fileName);
                File f; f.contentType = e.itemType;
                std::vector<uint8_t> inflated;
                if (e.compressionType != 0xFF && Zlib::decompress(itemIt->second.data(), itemIt->second.size(), inflated, e.originalSize))
                    f.data = std::make_shared<const std::vector<uint8_t>>(std::move(inflated));
                else
                    f.data = std::make_shared<const std::vector<uint8_t>>(itemIt->second);
                for (auto& path : paths) { auto nn = normalize(path); if (!files_.count(nn)) files_[nn] = f; }
            }
        }
    }

    static std::string normalize(const std::string& path) {
        std::string s = path;
        while (!s.empty() && s[0] == '/') s.erase(0, 1);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // ========================= 状態 =========================
    mutable std::mutex mutex_;
    SectionBuf pat_;
    std::unordered_map<uint16_t, SectionBuf> sections_; // PMT/D4KA PID 用
    std::unordered_map<uint16_t, bool> pmtPids_;
    std::optional<uint16_t> d4kaPid_;

    AppInfo ait_;
    std::string basePath_;
    std::unordered_map<uint16_t, std::string> nodeTagToPath_;
    std::unordered_map<uint16_t, std::string> dirNodeTagToPath_;
    std::map<MpuKey, DamtMpu> damtMpus_;
    std::map<ItemKey, std::vector<uint8_t>> items_;
    std::map<MpuKey, std::vector<IndexEntry>> indexes_;
    std::unordered_map<std::string, File> files_;
    std::unordered_map<uint16_t, ItemAssembly> itemAssembly_;
};

} // namespace AppDataTs
