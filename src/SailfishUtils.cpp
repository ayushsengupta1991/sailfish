/**
>HEADER
    Copyright (c) 2013 Rob Patro robp@cs.cmu.edu

    This file is part of Sailfish.

    Sailfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Sailfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Sailfish.  If not, see <http://www.gnu.org/licenses/>.
<HEADER
**/

#include <boost/thread/thread.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <iostream>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/range/join.hpp>

#include "spdlog/spdlog.h"

#include "gff.h"

#include "jellyfish/stream_manager.hpp"
#include "jellyfish/whole_sequence_parser.hpp"
#include "jellyfish/mer_dna.hpp"

#include "TranscriptGeneMap.hpp"
#include "GenomicFeature.hpp"
#include "SailfishUtils.hpp"
#include "ReadExperiment.hpp"

namespace sailfish {
    namespace utils {

        using std::string;
        using NameVector = std::vector<string>;
        using IndexVector = std::vector<size_t>;
        using KmerVector = std::vector<uint64_t>;


        void writeAbundancesFromCollapsed(
                const SailfishOpts& sopt,
                ReadExperiment& readExp,
                boost::filesystem::path& fname,
                std::string headerComments) {
            using sailfish::math::LOG_0;
            using sailfish::math::LOG_1;

            bool useScaledCounts = (sopt.allowOrphans == false);

            std::unique_ptr<std::FILE, int (*)(std::FILE *)> output(std::fopen(fname.c_str(), "w"), std::fclose);

            fmt::print(output.get(), "{}", headerComments);
            fmt::print(output.get(), "# Name\tLength\tTPM\tNumReads\n");

            double numMappedFrags = readExp.numMappedFragments();

            std::vector<Transcript>& transcripts_ = readExp.transcripts();
            for (auto& transcript : transcripts_) {
                transcript.projectedCounts = useScaledCounts ?
                    (transcript.mass() * numMappedFrags) : transcript.estCount();
            }

            double tfracDenom{0.0};
            for (auto& transcript : transcripts_) {
                double refLength = sopt.noEffectiveLengthCorrection ?
                    transcript.RefLength :
                    transcript.EffectiveLength;
                tfracDenom += (transcript.projectedCounts / numMappedFrags) / refLength;
            }

            double million = 1000000.0;
            // Now posterior has the transcript fraction
            for (auto& transcript : transcripts_) {
                double refLength = sopt.noEffectiveLengthCorrection ?
                    transcript.RefLength :
                    transcript.EffectiveLength;
                double count = transcript.projectedCounts;
                double npm = (transcript.projectedCounts / numMappedFrags);
                double tfrac = (npm / refLength) / tfracDenom;
                double tpm = tfrac * million;
                auto effLen = sopt.noEffectiveLengthCorrection ?
                    transcript.RefLength :
                    transcript.EffectiveLength;
                fmt::print(output.get(), "{}\t{}\t{}\t{}\n",
                        transcript.RefName, transcript.RefLength, //effLen,
                        tpm, count);
            }

        }

        /**
         * This function parses the library format string that specifies the format in which
         * the reads are to be expected.
         */
        LibraryFormat parseLibraryFormatString(std::string& fmt) {
            using std::vector;
            using std::string;
            using std::map;
            using std::stringstream;

            map<string, LibraryFormat> formatMap = {
                {"IU", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::U)},
                {"ISF", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::SA)},
                {"ISR", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::AS)},
                {"OU", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::AWAY, ReadStrandedness::U)},
                {"OSF", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::AWAY, ReadStrandedness::SA)},
                {"OSR", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::AWAY, ReadStrandedness::AS)},
                {"MU", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::SAME, ReadStrandedness::U)},
                {"MSF", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::SAME, ReadStrandedness::S)},
                {"MSR", LibraryFormat(ReadType::PAIRED_END, ReadOrientation::SAME, ReadStrandedness::A)},
                {"U", LibraryFormat(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::U)},
                {"SF", LibraryFormat(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::S)},
                {"SR", LibraryFormat(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::A)}};

            // inspired by http://stackoverflow.com/questions/236129/how-to-split-a-string-in-c
            // first convert the string to upper-case
            for (auto& c : fmt) { c = std::toupper(c); }


            auto libFmtIt = formatMap.find(fmt);

            if (libFmtIt == formatMap.end()) {
                stringstream errstr;
                errstr << "unknown library format string : " << fmt;
                throw std::invalid_argument(errstr.str());
            }

            return libFmtIt->second;
        }

        /**
         * Parses a set of __ordered__ command line options and extracts the relevant
         * read libraries from them.
         */
        std::vector<ReadLibrary> extractReadLibraries(boost::program_options::parsed_options& orderedOptions) {
            // The current (default) format for paired end data
            LibraryFormat peFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::U);
            // The current (default) format for single end data
            LibraryFormat seFormat(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::U);

            std::vector<ReadLibrary> peLibs{ReadLibrary(peFormat)};
            std::vector<ReadLibrary> seLibs{ReadLibrary(seFormat)};
            for (auto& opt : orderedOptions.options) {
                // Update the library type
                if (opt.string_key == "libType") {
                    auto libFmt = parseLibraryFormatString(opt.value[0]);
                    if (libFmt.type == ReadType::PAIRED_END) {
                        peFormat = libFmt;
                        peLibs.emplace_back(libFmt);
                    } else {
                        seFormat = libFmt;
                        seLibs.emplace_back(libFmt);
                    }
                }
                if (opt.string_key == "mates1") {
                    peLibs.back().addMates1(opt.value);
                }
                if (opt.string_key == "mates2") {
                    peLibs.back().addMates2(opt.value);
                }
                if (opt.string_key == "unmatedReads") {
                    seLibs.back().addUnmated(opt.value);
                }
            }

            std::vector<ReadLibrary> libs;
            libs.reserve(peLibs.size() + seLibs.size());
            for (auto& lib : boost::range::join(seLibs, peLibs)) {
                if (lib.format().type == ReadType::SINGLE_END) {
                    if (lib.unmated().size() == 0) {
                        // Didn't use default single end library type
                        continue;
                    }
                } else if (lib.format().type == ReadType::PAIRED_END) {
                    if (lib.mates1().size() == 0 or lib.mates2().size() == 0) {
                        // Didn't use default paired-end library type
                        continue;
                    }
                }
                libs.push_back(lib);
            }
            size_t numLibs = libs.size();
            std::cerr << "there " << ((numLibs > 1) ? "are " : "is ") << libs.size() << ((numLibs > 1) ? " libs\n" : " lib\n");
            return libs;
        }


        // for single end reads or orphans
        bool compatibleHit(LibraryFormat expected,
                           int32_t start, bool isForward, MateStatus ms) {
            auto expectedStrand = expected.strandedness;
            switch (ms) {
                case MateStatus::SINGLE_END:
                    if (isForward) { // U, SF
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::S);
                    } else { // U, SR
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::A);
                    }
                    break;
                case MateStatus::PAIRED_END_LEFT:
                    if (isForward) { // IU, ISF, OU, OSF, MU, MSF
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::S);
                    } else { // IU, ISR, OU, OSR, MU, MSR
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::A);
                    }
                    break;
                case MateStatus::PAIRED_END_RIGHT:
                    if (isForward) { // IU, ISR, OU, OSR, MU, MSR
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::A);
                    } else { // IU, ISF, OU, OSF, MU, MSF
                        return (expectedStrand == ReadStrandedness::U or
                                expectedStrand == ReadStrandedness::S);
                    }
                    break;
                default:
                    // SHOULD NOT GET HERE
                    fmt::print(stderr, "WARNING: Could not associate known library type with read!\n");
                    return false;
                    break;
            }
            // SHOULD NOT GET HERE
            fmt::print(stderr, "WARNING: Could not associate known library type with read!\n");
            return false;
        }


        // for paired-end reads
        bool compatibleHit(LibraryFormat expected, LibraryFormat observed) {
            if (observed.type != ReadType::PAIRED_END) {
                // SHOULD NOT GET HERE
                fmt::print(stderr, "WARNING: PE compatibility function called with SE read!\n");
                return false;
            }

            auto es = expected.strandedness;
            auto eo = expected.orientation;

            auto os = observed.strandedness;
            auto oo = observed.orientation;

            // If the orientations are different, they are incompatible
            if (eo != oo) {
                return false;
            } else { // In this branch, the orientations are always compatible
                return (es == ReadStrandedness::U or
                        es == os);
            }
            // SHOULD NOT GET HERE
            fmt::print(stderr, "WARNING: Could not determine strand compatibility!");
            fmt::print(stderr, "please report this.\n");
            return false;
        }


        // Determine the library type of paired-end reads
        LibraryFormat hitType(int32_t end1Start, bool end1Fwd, uint32_t len1,
                              int32_t end2Start, bool end2Fwd, uint32_t len2, bool canDovetail) {

            // If the reads come from opposite strands
            if (end1Fwd != end2Fwd) {
                // and if read 1 comes from the forward strand
                if (end1Fwd) {
                    // then if read 1 start < read 2 start ==> ISF
                    // NOTE: We can't really delineate between inward facing reads that stretch
                    // past each other and outward facing reads --- the purpose of stretch is to help
                    // make this determinateion.
                    int32_t stretch = canDovetail ? len2 : 0;
                    if (end1Start <= end2Start + stretch) {
                        return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::SA);
                    } // otherwise read 2 start < read 1 start ==> OSF
                    else {
                        return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::AWAY, ReadStrandedness::SA);
                    }
                }
                // and if read 2 comes from the forward strand
                if (end2Fwd) {
                    // then if read 2 start <= read 1 start ==> ISR
                    // NOTE: We can't really delineate between inward facing reads that stretch
                    // past each other and outward facing reads --- the purpose of stretch is to help
                    // make this determinateion.
                    int32_t stretch = canDovetail ? len1 : 0;
                    if (end2Start <= end1Start + stretch) {
                        return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::TOWARD, ReadStrandedness::AS);
                    } // otherwise, read 2 start > read 1 start ==> OSR
                    else {
                        return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::AWAY, ReadStrandedness::AS);
                    }
                }
            } else { // Otherwise, the reads come from the same strand
                if (end1Fwd) { // if it's the forward strand ==> MSF
                    return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::SAME, ReadStrandedness::S);
                } else { // if it's the reverse strand ==> MSR
                    return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::SAME, ReadStrandedness::A);
                }
            }
            // SHOULD NOT GET HERE
            spdlog::get("jointLog")->error("ERROR: Could not associate any known library type with read! "
                                           "Please report this bug!\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::exit(-1);
            return LibraryFormat(ReadType::PAIRED_END, ReadOrientation::NONE, ReadStrandedness::U);
        }

        uint64_t encode(uint64_t tid, uint64_t offset) {
            uint64_t res = (((tid & 0xFFFFFFFF) << 32) | (offset & 0xFFFFFFFF));
            return res;
        }

        uint32_t transcript(uint64_t enc) {
            uint32_t t = (enc & 0xFFFFFFFF00000000) >> 32;
            return t;
        }

        uint32_t offset(uint64_t enc) {
            uint32_t o = enc & 0xFFFFFFFF;
            return o;
        }

        size_t numberOfReadsInFastaFile(const std::string& fname) {
            constexpr size_t bufferSize = 16184;
            char buffer[bufferSize];
            std::ifstream ifile(fname, std::ifstream::in);
            ifile.rdbuf()->pubsetbuf(buffer, bufferSize);

            size_t numReads = 0;
            std::string s;
            while (ifile >> s) { if (s.front() == '>') { ++numReads; } }

            ifile.close();

            return numReads;
        }


        TranscriptGeneMap transcriptGeneMapFromGTF(const std::string& fname, std::string key) {

            using std::unordered_set;
            using std::unordered_map;
            using std::vector;
            using std::tuple;
            using std::string;
            using std::get;

            // Use GffReader to read the file
            GffReader reader(const_cast<char*>(fname.c_str()));
            // Remember the optional attributes
            reader.readAll(true);

            struct TranscriptKeyPair {
                const char* transcript_id;
                const char* key;
                TranscriptKeyPair(const char* t, const char* k) :
                    transcript_id(t), key(k) {}
            };

            // The user can group transcripts by gene_id, gene_name, or
            // an optinal attribute that they provide as a string.
            enum class TranscriptKey { GENE_ID, GENE_NAME, DYNAMIC };

            // Select the proper attribute by which to group
            TranscriptKey tkey = TranscriptKey::GENE_ID;

            if (key == "gene_id") {
            } else if (key == "gene_name") {
                tkey = TranscriptKey::GENE_NAME;
            } else {
                tkey = TranscriptKey::DYNAMIC;
            }

            // Iterate over all transcript features and build the
            // transcript <-> key vector.
            auto nfeat = reader.gflst.Count();
            std::vector<TranscriptKeyPair> feats;
            for (int i=0; i < nfeat; ++i) {
                auto f = reader.gflst[i];
                if (f->isTranscript()) {
                    const char* keyStr;
                    switch (tkey) {
                        case TranscriptKey::GENE_ID:
                            keyStr = f->getGeneID();
                            break;
                        case TranscriptKey::GENE_NAME:
                            keyStr = f->getGeneName();
                            break;
                        case TranscriptKey::DYNAMIC:
                            keyStr = f->getAttr(key.c_str());
                            break;
                    }
                    feats.emplace_back(f->getID(), keyStr);
                }
            }

            // Given the transcript <-> key vector, build the
            // TranscriptGeneMap.

            IndexVector t2g;
            NameVector transcriptNames;
            NameVector geneNames;

            // holds the mapping from transcript ID to gene ID
            IndexVector t2gUnordered;
            // holds the set of gene IDs
            unordered_map<string, size_t> geneNameToID;

            // To read the input and assign ids
            size_t transcriptCounter = 0;
            size_t geneCounter = 0;
            string transcript;
            string gene;

            std::sort( feats.begin(), feats.end(),
                    []( const TranscriptKeyPair & a, const TranscriptKeyPair & b) -> bool {
                    return std::strcmp(a.transcript_id, b.transcript_id) < 0;
                    } );

            std::string currentTranscript = "";
            for ( auto & feat : feats ) {

                std::string gene(feat.key);
                std::string transcript(feat.transcript_id);

                if ( transcript != currentTranscript ) {
                    auto geneIt = geneNameToID.find(gene);
                    size_t geneID = 0;

                    if ( geneIt == geneNameToID.end() ) {
                        // If we haven't seen this gene yet, give it a new ID
                        geneNameToID[gene] = geneCounter;
                        geneID = geneCounter;
                        geneNames.push_back(gene);
                        ++geneCounter;
                    } else {
                        // Otherwise lookup the ID
                        geneID = geneIt->second;
                    }

                    transcriptNames.push_back(transcript);
                    t2g.push_back(geneID);

                    //++transcriptID;
                    currentTranscript = transcript;
                }

            }

            return TranscriptGeneMap(transcriptNames, geneNames, t2g);

        }


        TranscriptGeneMap readTranscriptToGeneMap( std::ifstream &ifile ) {

            using std::unordered_set;
            using std::unordered_map;
            using std::vector;
            using std::tuple;
            using std::string;
            using std::get;

            using NameID = tuple<string, size_t>;

            IndexVector t2g;
            NameVector transcriptNames;
            NameVector geneNames;

            // holds the transcript name ID mapping
            vector<NameID> transcripts;
            // holds the mapping from transcript ID to gene ID
            IndexVector t2gUnordered;
            // holds the set of gene IDs
            unordered_map<string, size_t> geneNameToID;

            // To read the input and assign ids
            size_t transcriptCounter = 0;
            size_t geneCounter = 0;
            string transcript;
            string gene;

            while ( ifile >> transcript >> gene ) {
                // The transcript and it's ID
                transcripts.push_back( make_tuple(transcript, transcriptCounter) );

                auto geneIt = geneNameToID.find(gene);
                size_t geneID = 0;

                if ( geneIt == geneNameToID.end() ) {
                    // If we haven't seen this gene yet, give it a new ID
                    geneNameToID[gene] = geneCounter;
                    geneID = geneCounter;
                    geneNames.push_back(gene);
                    ++geneCounter;
                } else {
                    // Otherwise lookup the ID
                    geneID = geneIt->second;
                }

                // Map the transcript to the gene in terms of their IDs
                t2gUnordered.push_back(geneID);

                ++transcriptCounter;
            }

            std::sort( transcripts.begin(), transcripts.end(),
                    []( const NameID & a, const NameID & b) -> bool { return get<0>(a) < get<0>(b); } );

            // Resize these vectors for fast access
            transcriptNames.resize(t2gUnordered.size());
            t2g.resize(t2gUnordered.size());

            for ( size_t newID = 0; newID < transcripts.size(); ++newID ) {
                // For each transcript, map it to the appropriate gene
                string oldName; size_t oldID;
                std::tie(oldName, oldID) = transcripts[newID];
                t2g[newID] = t2gUnordered[oldID];
                transcriptNames[newID] = oldName;
            }

            return TranscriptGeneMap(transcriptNames, geneNames, t2g);
        }


        TranscriptGeneMap transcriptToGeneMapFromFasta( const std::string& transcriptsFile ) {
            using std::vector;
            using stream_manager = jellyfish::stream_manager<char**>;
            using sequence_parser = jellyfish::whole_sequence_parser<stream_manager>;
            namespace bfs = boost::filesystem;

            NameVector transcriptNames;
            NameVector geneNames {"gene"};

            vector<bfs::path> paths{transcriptsFile};

            // Create a jellyfish parser
            const int concurrentFile{1};
            char** fnames = new char*[1];
            fnames[0] = const_cast<char*>(transcriptsFile.c_str());
            stream_manager streams(fnames, fnames + 1, concurrentFile);

            size_t maxReadGroupSize{100};
            sequence_parser parser(4, maxReadGroupSize, concurrentFile, streams);

            // while there are transcripts left to process
            while (true) {
                sequence_parser::job j(parser);
                // If this job is empty, then we're done
                if (j.is_empty()) { break; }

                for (size_t i=0; i < j->nb_filled; ++i) {
                    // The transcript name
                    std::string fullHeader(j->data[i].header);
                    std::string header = fullHeader.substr(0, fullHeader.find(' '));
                    transcriptNames.emplace_back(header);
                }
            }

            // Sort the transcript names
            std::sort(transcriptNames.begin(), transcriptNames.end());

            // Since we have no real gene groupings, the t2g vector is trivial,
            // everything maps to gene 0.
            IndexVector t2g(transcriptNames.size(), 0);

            return TranscriptGeneMap(transcriptNames, geneNames, t2g);
        }

        class ExpressionRecord {
            public:
                ExpressionRecord(const std::string& targetIn, uint32_t lengthIn,
                        std::vector<double>& expValsIn) :
                    target(targetIn), length(lengthIn), expVals(expValsIn) {}

                ExpressionRecord( ExpressionRecord&& other ) {
                    std::swap(target, other.target);
                    length = other.length;
                    std::swap(expVals, other.expVals);
                }

                ExpressionRecord(std::vector<std::string>& inputLine) {
                    if (inputLine.size() < 3) {
                        std::string err ("Any expression line must contain at least 3 tokens");
                        throw std::invalid_argument(err);
                    } else {
                        auto it = inputLine.begin();
                        target = *it; ++it;
                        length = std::stoi(*it); ++it;
                        for (; it != inputLine.end(); ++it) {
                            expVals.push_back(std::stod(*it));
                        }
                    }
                }

                std::string target;
                uint32_t length;
                std::vector<double> expVals;
        };

        // From : http://stackoverflow.com/questions/9435385/split-a-string-using-c11
        std::vector<std::string> split(const std::string& str, int delimiter(int) = ::isspace){
            using namespace std;
            vector<string> result;
            auto e=str.end();
            auto i=str.begin();
            while (i != e) {
                i = find_if_not(i,e, delimiter);
                if (i == e) break;
                auto j = find_if(i,e, delimiter);
                result.push_back(string(i,j));
                i = j;
            }
            return result;
        }

        void aggregateEstimatesToGeneLevel(TranscriptGeneMap& tgm, boost::filesystem::path& inputPath) {

            using std::vector;
            using std::string;
            using std::ofstream;
            using std::unordered_map;
            using std::move;
            using std::cerr;
            using std::max;

            constexpr double minTPM = std::numeric_limits<double>::denorm_min();


            std::ifstream expFile(inputPath.string());

            if (!expFile.is_open()) {
                perror("Error reading file");
            }

            //====================== From GeneSum ====================
            vector<string> comments;
            unordered_map<string, vector<ExpressionRecord>> geneExps;
            string l;
            size_t ln{0};

            while (getline(expFile, l)) {
                if (++ln % 1000 == 0) {
                    cerr << "\r\rParsed " << ln << " expression lines";
                }
                auto it = find_if(l.begin(), l.end(),
                        [](char c) -> bool {return !isspace(c);});
                if (it != l.end()) {
                    if (*it == '#') {
                        comments.push_back(l);
                    } else {
                        vector<string> toks = split(l);
                        ExpressionRecord er(toks);
                        auto gn = tgm.geneName(er.target);
                        geneExps[gn].push_back(move(er));
                    }
                }
            }
            cerr << "\ndone\n";
            expFile.close();

            cerr << "Aggregating expressions to gene level . . .";
            boost::filesystem::path outputFilePath(inputPath);
            outputFilePath.replace_extension(".genes.sf");
            ofstream outFile(outputFilePath.string());

            // preserve any comments in the output
            for (auto& c : comments) {
                outFile << c << '\n';
            }

            for (auto& kv : geneExps) {
                auto& gn = kv.first;

                double geneLength = kv.second.front().length;
                vector<double> expVals(kv.second.front().expVals.size(), 0);
                const size_t NE{expVals.size()};

                size_t tpmIdx{0};
                double totalTPM{0.0};
                for (auto& tranExp : kv.second) {
                    // expVals[0] = TPM
                    // expVals[1] = count
                    for (size_t i = 0; i < NE; ++i) { expVals[i] += tranExp.expVals[i]; }
                    totalTPM += expVals[tpmIdx];
                }

                // If this gene was expressed
                if (totalTPM > minTPM) {
                    geneLength = 0.0;
                    for (auto& tranExp : kv.second) {
                        double frac = tranExp.expVals[tpmIdx] / totalTPM;
                        geneLength += tranExp.length * frac;
                    }
                } else {
                    geneLength = 0.0;
                    double frac = 1.0 / kv.second.size();
                    for (auto& tranExp : kv.second) {
                        geneLength += tranExp.length * frac;
                    }
                }

                // Otherwise, if the gene wasn't expressed, the length
                // is reported as the longest transcript length.

                outFile << gn << '\t' << geneLength;
                for (size_t i = 0; i < NE; ++i) {
                    outFile << '\t' << expVals[i];
                }
                outFile << '\n';
            }

            outFile.close();
            cerr << " done\n";
            //====================== From GeneSum =====================
        }

        void generateGeneLevelEstimates(boost::filesystem::path& geneMapPath,
                boost::filesystem::path& estDir,
                std::string aggKey,
                bool haveBiasCorrectedFile) {
            namespace bfs = boost::filesystem;
            std::cerr << "Computing gene-level abundance estimates\n";
            bfs::path gtfExtension(".gtf");
            auto extension = geneMapPath.extension();

            TranscriptGeneMap tranGeneMap;
            // parse the map as a GTF file
            if (extension == gtfExtension) {
                // Using libgff
                tranGeneMap = sailfish::utils::transcriptGeneMapFromGTF(geneMapPath.string(), aggKey);
            } else { // parse the map as a simple format files
                std::ifstream tgfile(geneMapPath.string());
                tranGeneMap = sailfish::utils::readTranscriptToGeneMap(tgfile);
                tgfile.close();
            }

            std::cerr << "There were " << tranGeneMap.numTranscripts() << " transcripts mapping to "
                << tranGeneMap.numGenes() << " genes\n";

            bfs::path estFilePath = estDir / "quant.sf";
            if (!bfs::exists(estFilePath)) {
                std::stringstream errstr;
                errstr << "Attempting to compute gene-level esimtates, but could not \n"
                    << "find isoform-level file " << estFilePath;
                throw std::invalid_argument(errstr.str());
            } else {
                sailfish::utils::aggregateEstimatesToGeneLevel(tranGeneMap, estFilePath);
            }

            /** Create a gene-level summary of the bias-corrected estimates as well if these exist **/
            if (haveBiasCorrectedFile) {
                bfs::path biasCorrectEstFilePath = estDir / "quant_bias_corrected.sf";
                if (!bfs::exists(biasCorrectEstFilePath)) {
                    std::stringstream errstr;
                    errstr << "Attempting to compute gene-level esimtates, but could not \n"
                        << "find bias-corrected isoform-level file " << biasCorrectEstFilePath;
                    throw std::invalid_argument(errstr.str());
                } else {
                    sailfish::utils::aggregateEstimatesToGeneLevel(tranGeneMap, biasCorrectEstFilePath);
                }
            }
        }

    }
}

//=======
