//
// Created by Ben Langmead on 2018-12-12.
//

#include <iostream>
#include <vector>
#include <cassert>
#include <sstream>
#include <algorithm>
#include <string>
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <htslib/sam.h>
#include <htslib/bgzf.h>

static const char USAGE[] = "BAM and BigWig utility.\n"
    "\n"
    "Usage:\n"
    "  bamcount <bam> [options]\n"
    "\n"
    "Options:\n"
    "  -h --help           Show this screen.\n"
    "  --version           Show version.\n"
    "  --read-ends         Print counts of read starts/ends\n"
    "  --coverage          Print per-base coverage (slow but totally worth it)\n"
    "  --alts              Print differing from ref per-base coverages\n"
    "   --include-softclip  Print a record for soft-clipped bases\n"
    "   --include-n         Print mismatch records when mismatched read base is N\n"
    "   --print-qual        Print quality values for mismatched bases\n"
    "   --delta             Print POS field as +/- delta from previous\n"
    "   --require-mdz       Quit with error unless MD:Z field exists everywhere it's\n"
    "                       expected\n"
    "  --no-head           Don't print sequence names and lengths in header\n"
    "  --echo-sam          Print a SAM record for each aligned read\n"
    "  --double-count      Allow overlapping ends of PE read to count twice toward\n"
    "                      coverage\n"
    "\n";

static const char* get_positional_n(const char ** begin, const char ** end, size_t n) {
    size_t i = 0;
    for(const char **itr = begin; itr != end; itr++) {
        if((*itr)[0] != '-') {
            if(i++ == n) {
                return *itr;
            }
        }
    }
    return nullptr;
}

static bool has_option(const char** begin, const char** end, const std::string& option) {
    return std::find(begin, end, option) != end;
}

/**
 * Holds an MDZ "operation"
 * op can be 
 */
struct MdzOp {
    char op;
    int run;
    char str[1024];
};

static inline std::ostream& seq_substring(std::ostream& os, const uint8_t *str, size_t off, size_t run) {
    for(size_t i = off; i < off + run; i++) {
        os << seq_nt16_str[bam_seqi(str, i)];
    }
    return os;
}

static inline std::ostream& kstring_out(std::ostream& os, const kstring_t *str) {
    for(size_t i = 0; i < str->l; i++) {
        os << str->s[i];
    }
    return os;
}

static inline std::ostream& cstr_substring(std::ostream& os, const uint8_t *str, size_t off, size_t run) {
    for(size_t i = off; i < off + run; i++) {
        os << (char)str[i];
    }
    return os;
}

/**
 * Parse given MD:Z extra field into a vector of MD:Z operations.
 */
static void parse_mdz(
        const uint8_t *mdz,
        std::vector<MdzOp>& ops)
{
    int i = 0;
    size_t mdz_len = strlen((char *)mdz);
    while(i < mdz_len) {
        if(isdigit(mdz[i])) {
            int run = 0;
            while(i < mdz_len && isdigit(mdz[i])) {
                run *= 10;
                run += (int)(mdz[i] - '0');
                i++;
            }
            if(run > 0) {
                ops.emplace_back(MdzOp{'=', run, ""});
                ops.back().str[0] = '\0';
            }
        } else if(isalpha(mdz[i])) {
            int st = i;
            while(i < mdz_len && isalpha(mdz[i])) i++;
            assert(i > st);
            ops.emplace_back(MdzOp{'X', i - st, ""});
            for(int j = 0; j < i ; j++) {
                ops.back().str[j] = mdz[st + j];
            }
            std::memcpy(ops.back().str, mdz + st, (size_t)(i - st));
            ops.back().str[i - st] = '\0';
        } else if(mdz[i] == '^') {
            i++;
            int st = i;
            while (i < mdz_len && isalpha(mdz[i])) i++;
            assert(i > st);
            ops.emplace_back(MdzOp{'^', i - st, ""});
            std::memcpy(ops.back().str, mdz + st, (size_t)(i - st));
            ops.back().str[i - st] = '\0';
        } else {
            std::stringstream ss;
            ss << "Unknown MD:Z operation: \"" << mdz[i] << "\"";
            throw std::runtime_error(ss.str());
        }
    }
}

#if 0
/**
 * Prints a stacked version of an alignment
 */
static void cigar_mdz_to_stacked(
        const BamAlignmentRecord& rec,
        vector<tuple<char, int, CharString>>& mdz,
        IupacString& rds,
        IupacString& rfs)
{
    const IupacString& seq{rec.seq};
    size_t mdz_off = 0, seq_off = 0;
    for (CigarElement<> e : rec.cigar) {
        const char &op = e.operation;
        bool ref_consuming = (strchr("DNMX=", op) != nullptr);
        if(ref_consuming && mdz_off >= mdz.size()) {
            stringstream ss;
            ss << "Found read-consuming CIGAR op after MD:Z had been exhausted" << endl;
            throw std::runtime_error(ss.str());
        }
        if (op == 'M' || op == 'X' || op == '=') {
            // Look for block matches and mismatches in MD:Z string
            size_t runleft = e.count;
            while (runleft > 0 && mdz_off < mdz.size()) {
                char mdz_op;
                size_t mdz_run;
                CharString mdz_str;
                std::tie(mdz_op, mdz_run, mdz_str) = mdz[mdz_off];
                size_t run_comb = std::min(runleft, mdz_run);
                runleft -= run_comb;
                assert(mdz_op == 'X' or mdz_op == '=');
                append(rds, infix(seq, seq_off, seq_off + run_comb));
                if (mdz_op == '=') {
                    append(rfs, infix(seq, seq_off, seq_off + run_comb));
                } else {
                    assert(length(mdz_str) == run_comb);
                    append(rfs, mdz_str);
                }
                seq_off += run_comb;
                if (run_comb < mdz_run) {
                    assert(mdz_op == '=');
                    get<1>(mdz[mdz_off]) -= run_comb;
                } else {
                    mdz_off++;
                }
            }
        } else if (op == 'I') {
            append(rds, infix(seq, seq_off, seq_off + e.count));
            for (size_t i = 0; i < e.count; i++) {
                append(rfs, '-');
            }
        } else if (op == 'D') {
            char mdz_op;
            size_t mdz_run;
            CharString mdz_str;
            std::tie(mdz_op, mdz_run, mdz_str) = mdz[mdz_off];
            assert(mdz_op == '^');
            assert(e.count == mdz_run);
            assert(length(mdz_str) == e.count);
            mdz_off++;
            for (size_t i = 0; i < e.count; i++) {
                append(rds, '-');
            }
            append(rfs, mdz_run);
        } else if (op == 'N') {
            for (size_t i = 0; i < e.count; i++) {
                append(rds, '-');
                append(rfs, '-');
            }
        } else if (op == 'S') {
            append(rds, infix(seq, seq_off, seq_off + e.count));
            for (size_t i = 0; i < e.count; i++) {
                append(rfs, '-');
            }
            seq_off += e.count;
        } else if (op == 'H') {
        } else if (op == 'P') {
        } else {
            stringstream ss;
            ss << "No such CIGAR operation as \"" << op << "\"";
            throw std::runtime_error(ss.str());
        }
    }
    assert(mdz_off == mdz.size());
}
#endif

static void output_from_cigar_mdz(
        const bam1_t *rec,
        std::vector<MdzOp>& mdz,
        bool print_qual = false,
        bool include_ss = false,
        bool include_n_mms = false,
        bool delta = false)
{
    uint8_t *seq = bam_get_seq(rec);
    uint8_t *qual = bam_get_qual(rec);
    // If QUAL field is *. this array is just a bunch of 255s
    uint32_t *cigar = bam_get_cigar(rec);
    size_t mdzi = 0, seq_off = 0;
    int32_t ref_off = rec->core.pos;
    for(int k = 0; k < rec->core.n_cigar; k++) {
        int op = bam_cigar_op(cigar[k]);
        int run = bam_cigar_oplen(cigar[k]);
        if((strchr("DNMX=", BAM_CIGAR_STR[op]) != nullptr) && mdzi >= mdz.size()) {
            std::stringstream ss;
            ss << "Found read-consuming CIGAR op after MD:Z had been exhausted" << std::endl;
            throw std::runtime_error(ss.str());
        }
        if(op == BAM_CMATCH || op == BAM_CDIFF || op == BAM_CEQUAL) {
            // Look for block matches and mismatches in MD:Z string
            int runleft = run;
            while(runleft > 0 && mdzi < mdz.size()) {
                int run_comb = std::min(runleft, mdz[mdzi].run);
                runleft -= run_comb;
                assert(mdz[mdzi].op == 'X' || mdz[mdzi].op == '=');
                if(mdz[mdzi].op == '=') {
                    // nop
                } else {
                    assert(mdz[mdzi].op == 'X');
                    assert(strlen(mdz[mdzi].str) == run_comb);
                    int cread = bam_seqi(seq, seq_off);
                    if(!include_n_mms && run_comb == 1 && cread == 'N') {
                        // skip
                    } else {
                        std::cout << rec->core.tid << ',' << ref_off << ",X,";
                        seq_substring(std::cout, seq, seq_off, (size_t)run_comb);
                        if(print_qual) {
                            std::cout << ',';
                            cstr_substring(std::cout, qual, seq_off, (size_t)run_comb);
                        }
                        std::cout << std::endl;
                    }
                }
                seq_off += run_comb;
                ref_off += run_comb;
                if(run_comb < mdz[mdzi].run) {
                    assert(mdz[mdzi].op == '=');
                    mdz[mdzi].run -= run_comb;
                } else {
                    mdzi++;
                }
            }
        } else if(op == BAM_CINS) {
            std::cout << rec->core.tid << ',' << ref_off << ",I,";
            seq_substring(std::cout, seq, seq_off, (size_t)run) << std::endl;
            seq_off += run;
        } else if(op == BAM_CSOFT_CLIP) {
            if(include_ss) {
                std::cout << rec->core.tid << ',' << ref_off << ",S,";
                seq_substring(std::cout, seq, seq_off, (size_t)run) << std::endl;
                seq_off += run;
            }
        } else if (op == BAM_CDEL) {
            assert(mdz[mdzi].op == '^');
            assert(run == mdz[mdzi].run);
            assert(strlen(mdz[mdzi].str) == run);
            mdzi++;
            std::cout << rec->core.tid << ',' << ref_off << ",D," << run << std::endl;
            ref_off += run;
        } else if (op == BAM_CREF_SKIP) {
            ref_off += run;
        } else if (op == BAM_CHARD_CLIP) {
        } else if (op == BAM_CPAD) {
        } else {
            std::stringstream ss;
            ss << "No such CIGAR operation as \"" << op << "\"";
            throw std::runtime_error(ss.str());
        }
    }
    assert(mdzi == mdz.size());
}

static void output_from_cigar(const bam1_t *rec) {
    uint8_t *seq = bam_get_seq(rec);
    uint32_t *cigar = bam_get_cigar(rec);
    uint32_t n_cigar = rec->core.n_cigar;
    if(n_cigar == 1) {
        return;
    }
    int32_t refpos = rec->core.pos;
    int32_t seqpos = 0;
    for(uint32_t k = 0; k < n_cigar; k++) {
        int op = bam_cigar_op(cigar[k]);
        int run = bam_cigar_oplen(cigar[k]);
        switch(op) {
            case BAM_CDEL: {
                std::cout << rec->core.tid << ',' << refpos << ",D," << run << std::endl;
                refpos += run;
                break;
            }
            case BAM_CSOFT_CLIP:
            case BAM_CINS: {
                std::cout << rec->core.tid << ',' << refpos << ',' << BAM_CIGAR_STR[op] << ',';
                seq_substring(std::cout, seq, (size_t)seqpos, (size_t)run) << std::endl;
                seqpos += run;
                break;
            }
            case BAM_CREF_SKIP: {
                refpos += run;
                break;
            }
            case BAM_CMATCH:
            case BAM_CDIFF:
            case BAM_CEQUAL: {
                seqpos += run;
                refpos += run;
                break;
            }
            case 'H':
            case 'P': { break; }
            default: {
                std::stringstream ss;
                ss << "No such CIGAR operation as \"" << op << "\"";
                throw std::runtime_error(ss.str());
            }
        }
    }
}

static void print_header(const bam_hdr_t * hdr) {
    for(int32_t i = 0; i < hdr->n_targets; i++) {
        std::cout << '@' << i << ','
                  << hdr->target_name[i] << ','
                  << hdr->target_len[i] << std::endl;
    }
}

static long get_longest_target_size(const bam_hdr_t * hdr) {
    long max = 0;
    for(int32_t i = 0; i < hdr->n_targets; i++) {
        if(hdr->target_len[i] > max)
            max = hdr->target_len[i];
    }
    return max;
}

static void reset_array(uint32_t* arr, const long arr_sz) {
    for(long i = 0; i < arr_sz; i++)
        arr[i] = 0;
}

static void print_array(const char* prefix, 
                        const uint32_t* arr, 
                        const long arr_sz,
                        const bool skip_zeros,
                        const bool collapse_regions) {
    bool first = true;
    uint32_t running_value = 0;
    long last_pos = 0;
    //this will print the coordinates in base-0
    for(long i = 0; i < arr_sz; i++) {
        if(first || running_value != arr[i])
        {
            if(!first)
                if(running_value > 0 || !skip_zeros)
                    fprintf(stdout, "%s\t%lu\t%lu\t%u\n", prefix, last_pos, i, running_value);
            first = false;
            running_value = arr[i];
            last_pos = i;
        }
    }
    if(!first)
        if(running_value > 0 || !skip_zeros)
            fprintf(stdout, "%s\t%lu\t%lu\t%u\n", prefix, last_pos, arr_sz, running_value);
}

static int32_t align_length(const bam1_t *rec)
{
    return bam_endpos(rec) - rec->core.pos;
    /*uint32_t *cigar = bam_get_cigar(rec);
    uint32_t n_cigar = rec->core.n_cigar;
    uint32_t algn_len = 0;
    for(uint32_t k = 0; k < n_cigar; k++) {
        int op = bam_cigar_op(cigar[k]);
        int run = bam_cigar_oplen(cigar[k]);
        switch(op) {
            case BAM_CMATCH: 
            case BAM_CDEL: 
            case BAM_CREF_SKIP:
            case BAM_CEQUAL:
            case BAM_CDIFF:
                algn_len += run;
       }
    }
    return algn_len;*/
}

static int32_t coverage(const bam1_t *rec, uint32_t* coverages, 
                        std::unordered_map<std::string, int>* overlapping_mates)
{
    int32_t refpos = rec->core.pos;
    //lifted from htslib's bam_cigar2rlen(...) & bam_endpos(...)
    int32_t pos = refpos;
    int32_t algn_end_pos = refpos;
    const uint32_t* cigar = bam_get_cigar(rec);
    int k, z;
    //check for overlapping mate and corect double counting if exists
    int32_t mate_end = -1;
    char* qname = bam_get_qname(rec);
    if(overlapping_mates->find(qname) != overlapping_mates->end() 
                        && (rec->core.flag & BAM_FPROPER_PAIR) == 2)
        mate_end = (*overlapping_mates)[qname];
    for (k = 0; k < rec->core.n_cigar; ++k)
    {
        if(bam_cigar_type(bam_cigar_op(cigar[k]))&2)
        {
            int32_t len = bam_cigar_oplen(cigar[k]);
            for(z = algn_end_pos; z < algn_end_pos + len; z++)
                if(z > mate_end)
                    coverages[z]++;
            algn_end_pos += len;
        }
    }

    //using the mosdepth approach to tracking overlapping mates
    if(rec->core.tid == rec->core.mtid && (rec->core.flag & BAM_FPROPER_PAIR) == 2
            && algn_end_pos > rec->core.mpos && rec->core.pos < rec->core.mpos)
        (*overlapping_mates)[qname] = algn_end_pos - 1;
    return algn_end_pos;
}
    

int main(int argc, const char** argv) {
    const char** argv_ptr = argv;
    const char *bam_arg_c_str = get_positional_n(++argv_ptr, argv+argc, 0);
    if(!bam_arg_c_str) {
        std::cerr << "ERROR: Could not find <bam> positional arg" << std::endl;
        return 1;
    }
    std::string bam_arg{bam_arg_c_str};

    htsFile *bam_fh = sam_open(bam_arg.c_str(), "r");
    if(!bam_fh) {
        std::cerr << "ERROR: Could not open " << bam_arg << ": "
                  << std::strerror(errno) << std::endl;
        return 1;
    }

    bool print_qual = has_option(argv, argv+argc, "--print-qual");
    const bool include_ss = has_option(argv, argv+argc, "--include-softclip");
    const bool include_n_mms = has_option(argv, argv+argc, "--include-n");

    size_t recs = 0;
    bam_hdr_t *hdr = sam_hdr_read(bam_fh);
    if(!hdr) {
        std::cerr << "ERROR: Could not read header for " << bam_arg
                  << ": " << std::strerror(errno) << std::endl;
        return 1;
    }
    if(!has_option(argv, argv+argc, "--no-head")) {
        print_header(hdr);
    }
    std::vector<MdzOp> mdzbuf;
    bam1_t *rec = bam_init1();
    if (!rec) {
        std::cerr << "ERROR: Could not initialize BAM object: "
                  << std::strerror(errno) << std::endl;
        return 1;
    }
    kstring_t sambuf{ 0, 0, nullptr };
    bool first = true;
    //largest human chromosome is ~249M bases
    //long chr_size = 250000000;
    long chr_size = -1;
    uint32_t* coverages;
    std::unordered_map<std::string, int> overlapping_mates; 
    if(has_option(argv, argv+argc, "--coverage")) {
        chr_size = get_longest_target_size(hdr);
        coverages = new uint32_t[chr_size];
    }
    char prefix[50]="";
    int32_t ptid = -1;
    uint32_t* starts;
    uint32_t* ends;
    if(has_option(argv, argv+argc, "--read-ends")) {
        if(chr_size == -1) 
            chr_size = get_longest_target_size(hdr);
        starts = new uint32_t[chr_size];
        ends = new uint32_t[chr_size];
    }

    while(sam_read1(bam_fh, hdr, rec) >= 0) {
        recs++;
        bam1_core_t *c = &rec->core;
        if((c->flag & BAM_FUNMAP) == 0) {
            //TODO track fragment lengths

            int32_t tid = rec->core.tid;
            int32_t end_refpos = -1;
            //track coverage
            if(has_option(argv, argv+argc, "--coverage")) {
                if(tid != ptid) {
                    if(ptid != -1) {
                        sprintf(prefix, "cov\t%d", ptid);
                        print_array(prefix, coverages, hdr->target_len[ptid], false, true);
                    }
                    reset_array(coverages, chr_size);
                }
                end_refpos = coverage(rec, coverages, &overlapping_mates);
            }

            //track read starts/ends
            if(has_option(argv, argv+argc, "--read-ends")) {
                int32_t refpos = rec->core.pos;
                if(tid != ptid) {
                    if(ptid != -1) {
                        sprintf(prefix, "start\t%d", ptid);
                        print_array(prefix, starts, hdr->target_len[ptid], true, true);
                        sprintf(prefix, "end\t%d", ptid);
                        print_array(prefix, ends, hdr->target_len[ptid], true, true);
                    }
                    reset_array(starts, chr_size);
                    reset_array(ends, chr_size);
                }
                starts[refpos]++;
                if(end_refpos == -1)
                    end_refpos = refpos + align_length(rec) - 1;
                ends[end_refpos]++;
            }
            ptid = tid;

            //echo back the sam record
            if(has_option(argv, argv+argc, "--echo-sam")) {
                int ret = sam_format1(hdr, rec, &sambuf);
                if(ret < 0) {
                    std::cerr << "Could not format SAM record: " << std::strerror(errno) << std::endl;
                    return 1;
                }
                kstring_out(std::cout, &sambuf);
                std::cout << std::endl;
            }
            //track alt. base coverages
            if(has_option(argv, argv+argc, "--alts")) {
                if(first) {
                    if(print_qual) {
                        uint8_t *qual = bam_get_qual(rec);
                        if(qual[0] == 255) {
                            std::cerr << "WARNING: --print-qual specified but quality strings don't seem to be present" << std::endl;
                            print_qual = false;
                        }
                    }
                    first = false;
                }
                const uint8_t *mdz = bam_aux_get(rec, "MD");
                if(!mdz) {
                    if(has_option(argv, argv+argc, "--require-mdz")) {
                        std::stringstream ss;
                        ss << "No MD:Z extra field for aligned read \"" << hdr->target_name[c->tid] << "\"";
                        throw std::runtime_error(ss.str());
                    }
                    output_from_cigar(rec); // just use CIGAR
                } else {
                    mdzbuf.clear();
                    parse_mdz(mdz + 1, mdzbuf); // skip type character at beginning
                    output_from_cigar_mdz(
                            rec, mdzbuf, print_qual,
                            include_ss, include_n_mms); // use CIGAR and MD:Z
                }
            }
        }
    }
    if(has_option(argv, argv+argc, "--coverage")) {
        if(ptid != -1) {
            sprintf(prefix, "cov\t%d", ptid);
            print_array(prefix, coverages, hdr->target_len[ptid], false, true);
        }
        delete coverages;
    }
    if(has_option(argv, argv+argc, "--read-ends")) {
        if(ptid != -1) {
            sprintf(prefix, "start\t%d", ptid);
            print_array(prefix, starts, hdr->target_len[ptid], true, true);
            sprintf(prefix, "end\t%d", ptid);
            print_array(prefix, ends, hdr->target_len[ptid], true, true);
        }
        delete starts;
        delete ends;
    }
    std::cout << "Read " << recs << " records" << std::endl;
    return 0;
}
