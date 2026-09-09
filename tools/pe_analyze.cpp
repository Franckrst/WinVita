// tools/pe_analyze — dump a PE32's imports, exports, sections, relocs.
// Used to build the hybrid-strategy DLL matrix: what each D2 DLL calls
// out to (Win32 side + other D2 DLLs), how big it is, and how complex.
//
// Output format is tab-separated blocks so we can post-process with awk
// or feed the raw output straight into a markdown table.
//
// Usage:
//   pe_analyze <path-to-pe>
//   pe_analyze --matrix <path1> <path2> ...

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

// -------- Minimal PE32 struct layouts (little-endian, packed) ---------------

#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;      // "MZ"
    uint8_t  pad[58];
    uint32_t e_lfanew;     // PE header offset
};

struct CoffHeader {
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t sym_table;
    uint32_t num_syms;
    uint16_t opt_size;
    uint16_t characteristics;
};

struct DataDirectory {
    uint32_t rva;
    uint32_t size;
};

struct OptHeader32 {
    uint16_t magic;             // 0x10b = PE32
    uint8_t  major_link, minor_link;
    uint32_t code_size;
    uint32_t init_data_size;
    uint32_t uninit_data_size;
    uint32_t entry_point;
    uint32_t base_of_code;
    uint32_t base_of_data;
    uint32_t image_base;
    uint32_t section_align;
    uint32_t file_align;
    uint16_t major_os, minor_os;
    uint16_t major_img, minor_img;
    uint16_t major_sub, minor_sub;
    uint32_t win32_version;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint32_t stack_reserve, stack_commit;
    uint32_t heap_reserve, heap_commit;
    uint32_t loader_flags;
    uint32_t num_rva;
    DataDirectory dirs[16];
};

struct SectionHeader {
    char     name[8];
    uint32_t virt_size;
    uint32_t virt_addr;
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t reloc_ptr;
    uint32_t linenum_ptr;
    uint16_t num_relocs;
    uint16_t num_linenums;
    uint32_t characteristics;
};

struct ImportDescriptor {
    uint32_t lookup_rva;
    uint32_t timestamp;
    uint32_t forwarder;
    uint32_t name_rva;
    uint32_t iat_rva;
};

struct ExportDirectory {
    uint32_t characteristics;
    uint32_t timestamp;
    uint16_t major, minor;
    uint32_t name_rva;
    uint32_t ordinal_base;
    uint32_t num_functions;
    uint32_t num_names;
    uint32_t functions_rva;
    uint32_t names_rva;
    uint32_t ordinals_rva;
};

struct BaseReloc {
    uint32_t page_rva;
    uint32_t block_size;
};
#pragma pack(pop)

// -------- File loader ---------------------------------------------------------

struct PeFile {
    std::vector<uint8_t> data;
    const DosHeader*     dos    = nullptr;
    const CoffHeader*    coff   = nullptr;
    const OptHeader32*   opt    = nullptr;
    const SectionHeader* sects  = nullptr;
    int                  n_sect = 0;
};

bool load_pe(const std::string& path, PeFile& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    out.data.resize(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data.data()), sz);
    if (out.data.size() < sizeof(DosHeader)) return false;
    out.dos = reinterpret_cast<const DosHeader*>(out.data.data());
    if (out.dos->e_magic != 0x5a4d) return false;   // MZ
    const uint32_t peoff = out.dos->e_lfanew;
    if (peoff + 4 + sizeof(CoffHeader) > out.data.size()) return false;
    if (std::memcmp(out.data.data() + peoff, "PE\0\0", 4) != 0) return false;
    out.coff = reinterpret_cast<const CoffHeader*>(out.data.data() + peoff + 4);
    out.opt  = reinterpret_cast<const OptHeader32*>(
        out.data.data() + peoff + 4 + sizeof(CoffHeader));
    if (out.opt->magic != 0x10b) {
        std::fprintf(stderr, "not PE32 (opt.magic=0x%x)\n", out.opt->magic);
        return false;
    }
    out.sects = reinterpret_cast<const SectionHeader*>(
        reinterpret_cast<const uint8_t*>(out.opt) + out.coff->opt_size);
    out.n_sect = out.coff->num_sections;
    return true;
}

// Translate an RVA into a raw file offset via the section table.
uint32_t rva_to_off(const PeFile& p, uint32_t rva) {
    for (int i = 0; i < p.n_sect; ++i) {
        const auto& s = p.sects[i];
        if (rva >= s.virt_addr && rva < s.virt_addr + s.virt_size) {
            return s.raw_ptr + (rva - s.virt_addr);
        }
    }
    return 0;
}

const char* c_at(const PeFile& p, uint32_t off) {
    if (off == 0 || off >= p.data.size()) return "";
    return reinterpret_cast<const char*>(p.data.data() + off);
}

// -------- Analysis passes ----------------------------------------------------

struct ImportEntry {
    std::string dll;
    std::string func;      // empty if ordinal
    uint16_t    ordinal = 0;
    bool        by_ordinal = false;
};

std::vector<ImportEntry> read_imports(const PeFile& p) {
    std::vector<ImportEntry> out;
    const auto& d = p.opt->dirs[1];    // IMAGE_DIRECTORY_ENTRY_IMPORT
    if (d.rva == 0 || d.size == 0) return out;
    const uint32_t base = rva_to_off(p, d.rva);
    if (!base) return out;
    auto* id = reinterpret_cast<const ImportDescriptor*>(
        p.data.data() + base);
    for (; id->name_rva; ++id) {
        std::string dll = c_at(p, rva_to_off(p, id->name_rva));
        uint32_t lookup = id->lookup_rva ? id->lookup_rva : id->iat_rva;
        if (!lookup) continue;
        auto* thunks = reinterpret_cast<const uint32_t*>(
            p.data.data() + rva_to_off(p, lookup));
        for (int i = 0; thunks[i]; ++i) {
            ImportEntry e;
            e.dll = dll;
            if (thunks[i] & 0x80000000u) {
                e.by_ordinal = true;
                e.ordinal    = thunks[i] & 0xffff;
            } else {
                uint32_t off = rva_to_off(p, thunks[i]);
                // Skip 2-byte hint prefix to reach the ASCII function name.
                e.func = c_at(p, off + 2);
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

struct ExportEntry {
    uint32_t    ordinal;
    uint32_t    rva;
    std::string name;      // empty if ordinal-only
    std::string forwarder; // e.g. "OTHER.func" if the export forwards
};

std::vector<ExportEntry> read_exports(const PeFile& p) {
    std::vector<ExportEntry> out;
    const auto& d = p.opt->dirs[0];    // IMAGE_DIRECTORY_ENTRY_EXPORT
    if (d.rva == 0 || d.size == 0) return out;
    const uint32_t base = rva_to_off(p, d.rva);
    if (!base) return out;
    const auto* ed = reinterpret_cast<const ExportDirectory*>(
        p.data.data() + base);
    if (ed->num_functions == 0) return out;
    const uint32_t* funcs =
        reinterpret_cast<const uint32_t*>(p.data.data() + rva_to_off(p, ed->functions_rva));
    // Build ordinal → RVA table.
    out.reserve(ed->num_functions);
    for (uint32_t i = 0; i < ed->num_functions; ++i) {
        ExportEntry e;
        e.ordinal = ed->ordinal_base + i;
        e.rva     = funcs[i];
        // Forwarder: RVA points inside the export dir → "DLL.name" string.
        if (e.rva >= d.rva && e.rva < d.rva + d.size) {
            e.forwarder = c_at(p, rva_to_off(p, e.rva));
        }
        out.push_back(e);
    }
    // Attach names for those that have them.
    if (ed->num_names) {
        const uint32_t* names =
            reinterpret_cast<const uint32_t*>(p.data.data() + rva_to_off(p, ed->names_rva));
        const uint16_t* ords =
            reinterpret_cast<const uint16_t*>(p.data.data() + rva_to_off(p, ed->ordinals_rva));
        for (uint32_t i = 0; i < ed->num_names; ++i) {
            uint32_t idx = ords[i];
            if (idx < out.size()) {
                out[idx].name = c_at(p, rva_to_off(p, names[i]));
            }
        }
    }
    return out;
}

// Count fix-ups declared in the .reloc base-relocation directory.
uint32_t count_relocs(const PeFile& p) {
    const auto& d = p.opt->dirs[5];    // IMAGE_DIRECTORY_ENTRY_BASERELOC
    if (d.rva == 0 || d.size == 0) return 0;
    uint32_t total = 0;
    const uint32_t base = rva_to_off(p, d.rva);
    if (!base) return 0;
    const uint8_t* end = p.data.data() + base + d.size;
    const uint8_t* cur = p.data.data() + base;
    while (cur < end) {
        auto* br = reinterpret_cast<const BaseReloc*>(cur);
        if (br->block_size < sizeof(BaseReloc)) break;
        total += (br->block_size - sizeof(BaseReloc)) / 2;
        cur += br->block_size;
    }
    return total;
}

// -------- Single-file pretty print -------------------------------------------

void dump_single(const std::string& path) {
    PeFile p;
    if (!load_pe(path, p)) {
        std::fprintf(stderr, "cannot load %s\n", path.c_str());
        return;
    }
    std::printf("=== %s ===\n", path.c_str());
    std::printf("machine=0x%04x sections=%d opt_size=%d\n",
                p.coff->machine, p.coff->num_sections, p.coff->opt_size);
    std::printf("image_base=0x%08x image_size=%u entry_rva=0x%08x subsystem=%u\n",
                p.opt->image_base, p.opt->image_size, p.opt->entry_point, p.opt->subsystem);
    std::printf("code=%u init_data=%u uninit_data=%u\n",
                p.opt->code_size, p.opt->init_data_size, p.opt->uninit_data_size);
    std::printf("relocs=%u\n\n", count_relocs(p));

    std::printf("Sections:\n");
    for (int i = 0; i < p.n_sect; ++i) {
        const auto& s = p.sects[i];
        char name[9] = {0};
        std::memcpy(name, s.name, 8);
        std::printf("  %-8s vrva=0x%08x vsize=%u  rsize=%u  flags=0x%08x\n",
                    name, s.virt_addr, s.virt_size, s.raw_size, s.characteristics);
    }
    std::printf("\n");

    auto imports = read_imports(p);
    std::map<std::string, std::vector<std::string>> by_dll;
    for (const auto& e : imports) {
        if (e.by_ordinal) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "#%u", e.ordinal);
            by_dll[e.dll].emplace_back(buf);
        } else {
            by_dll[e.dll].push_back(e.func);
        }
    }
    std::printf("Imports (%zu across %zu dlls):\n", imports.size(), by_dll.size());
    for (auto& [dll, fns] : by_dll) {
        std::sort(fns.begin(), fns.end());
        std::printf("  %s (%zu):\n", dll.c_str(), fns.size());
        for (const auto& f : fns) std::printf("    %s\n", f.c_str());
    }
    std::printf("\n");

    auto exports = read_exports(p);
    std::printf("Exports (%zu functions):\n", exports.size());
    int by_name = 0, by_ord = 0, forwarded = 0;
    for (const auto& e : exports) {
        if (!e.forwarder.empty()) ++forwarded;
        if (!e.name.empty()) ++by_name; else ++by_ord;
    }
    std::printf("  named=%d ordinal-only=%d forwarded=%d\n",
                by_name, by_ord, forwarded);
    if (exports.size() < 40) {
        for (const auto& e : exports) {
            if (!e.name.empty())
                std::printf("    @%u  %s\n", e.ordinal, e.name.c_str());
            else
                std::printf("    @%u  (ordinal-only)  rva=0x%08x\n", e.ordinal, e.rva);
        }
    } else {
        std::printf("  (truncated; %zu exports total — pass --matrix for summary)\n",
                    exports.size());
    }
}

// -------- Matrix mode --------------------------------------------------------

struct Row {
    std::string name;
    uint32_t    size_bytes;
    uint32_t    code_bytes;
    uint32_t    data_bytes;
    int         n_exports;
    int         n_imports;
    std::set<std::string> import_dlls;
    // Split Win32 vs D2:
    int         win32_imports;
    int         d2_imports;
};

bool is_d2_dll(const std::string& name) {
    // Case-insensitive suffix and prefix match.
    std::string n;
    n.reserve(name.size());
    for (char c : name) n.push_back((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return n.rfind("d2", 0) == 0 || n == "fog.dll" || n == "storm.dll" ||
           n == "bnclient.dll" || n == "ijl11.dll";
}

void matrix_mode(const std::vector<std::string>& paths) {
    std::vector<Row> rows;
    for (const auto& p : paths) {
        PeFile pe;
        if (!load_pe(p, pe)) { std::fprintf(stderr, "skip %s\n", p.c_str()); continue; }
        Row r;
        r.name       = p.substr(p.find_last_of('/') + 1);
        r.size_bytes = pe.opt->image_size;
        r.code_bytes = pe.opt->code_size;
        r.data_bytes = pe.opt->init_data_size + pe.opt->uninit_data_size;
        auto exports = read_exports(pe);
        r.n_exports  = exports.size();
        auto imports = read_imports(pe);
        r.n_imports  = imports.size();
        r.win32_imports = r.d2_imports = 0;
        for (const auto& e : imports) {
            r.import_dlls.insert(e.dll);
            if (is_d2_dll(e.dll)) ++r.d2_imports;
            else                  ++r.win32_imports;
        }
        rows.push_back(r);
    }
    // Sort by size descending.
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.size_bytes > b.size_bytes; });

    std::printf("| DLL | Image | Code | Data | Exports | Imports | Win32 | D2 | Depends on |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---|\n");
    for (const auto& r : rows) {
        std::string deps;
        for (const auto& d : r.import_dlls) {
            if (!deps.empty()) deps += ", ";
            deps += d;
        }
        std::printf("| %s | %uK | %uK | %uK | %d | %d | %d | %d | %s |\n",
                    r.name.c_str(),
                    r.size_bytes / 1024, r.code_bytes / 1024, r.data_bytes / 1024,
                    r.n_exports, r.n_imports, r.win32_imports, r.d2_imports,
                    deps.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <pe>\n       %s --matrix <pe1> <pe2> ...\n",
                     argv[0], argv[0]);
        return 2;
    }
    if (std::string(argv[1]) == "--matrix") {
        std::vector<std::string> ps;
        for (int i = 2; i < argc; ++i) ps.emplace_back(argv[i]);
        matrix_mode(ps);
        return 0;
    }
    dump_single(argv[1]);
    return 0;
}
