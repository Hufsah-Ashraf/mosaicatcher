/*
Copyright (C) 2016 Sascha Meiers
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Sascha Meiers (meiers@embl.de)
============================================================================
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <tuple>

#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/tokenizer.hpp>
#include <boost/filesystem.hpp>
#include <boost/progress.hpp>
#include <htslib/sam.h>

#include "intervals.hpp"
#include "counts.hpp"
#include "distribution.hpp"
#include "hmm.hpp"



struct Conf {
    std::vector<boost::filesystem::path> f_in;
    boost::filesystem::path f_out;
    boost::filesystem::path f_bins;
    boost::filesystem::path f_excl;
    boost::filesystem::path f_info;
    boost::filesystem::path f_sample_info;
    boost::filesystem::path f_removed_bins;
    int minMapQual;
    unsigned int window;
    std::string mode;
};

std::vector<unsigned> median_by_sample(std::vector<TGenomeCounts> & counts)
{
    std::vector<unsigned> median_by_sample(counts.size());
    for(unsigned i = 0; i < counts.size(); ++i) {
        TMedianAccumulator<unsigned int> med_acc;
        for (Counter const & count_bin : counts[i])
            med_acc(count_bin.watson_count + count_bin.crick_count);
        median_by_sample[i] = boost::accumulators::median(med_acc);
    }
    return median_by_sample;
}





int main(int argc, char **argv)
{

    // Command line options
    Conf conf;
    boost::program_options::options_description generic("Generic options");
    generic.add_options()
    ("help,?", "show help message")
    ("mapq,q", boost::program_options::value<int>(&conf.minMapQual)->default_value(10), "min mapping quality")
    ("window,w", boost::program_options::value<unsigned int>(&conf.window)->default_value(1000000), "window size of fixed windows")
    ("out,o", boost::program_options::value<boost::filesystem::path>(&conf.f_out)->default_value("out.txt"), "output file for counts")
    ("bins,b", boost::program_options::value<boost::filesystem::path>(&conf.f_bins), "variable bin file (BED format, mutually exclusive to -w)")
    ("exclude,x", boost::program_options::value<boost::filesystem::path>(&conf.f_excl), "Exclude chromosomes (mutually exclusive to -b)")
    ("info,i", boost::program_options::value<boost::filesystem::path>(&conf.f_info), "Write info about samples")
    ;

    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
    ("input-file", boost::program_options::value<std::vector<boost::filesystem::path> >(&conf.f_in), "input bam file(s)")
    ("sample_info,S", boost::program_options::value<boost::filesystem::path>(&conf.f_sample_info),   "write info per sample")
    ("removed_bins,R", boost::program_options::value<boost::filesystem::path>(&conf.f_removed_bins), "bins that were removed (bed file)")
    ;

    boost::program_options::positional_options_description pos_args;
    pos_args.add("input-file", -1);

    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic).add(hidden);
    boost::program_options::options_description visible_options;
    visible_options.add(generic);
    boost::program_options::variables_map vm;

    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
    boost::program_options::notify(vm);

    // Check command line arguments
    if (!vm["window"].defaulted() && vm.count("bins")) {
        std::cerr << "Error: -w and -b cannot be specified together" << std::endl << std::endl;
        goto print_usage_and_exit;
    }
    if (vm.count("bins") && vm.count("exclude")) {
        std::cerr << "Error: Exclude chromosomes (-x) have no effect when -b is specified. Stop" << std::endl << std::endl;
        goto print_usage_and_exit;
    }

    if (vm.count("help") || !vm.count("input-file"))
    {
    print_usage_and_exit:
        std::cout << "Usage: " << argv[0] << " [OPTIONS] <strand.seq1.bam> <strand.seq2.bam> ... <strand.seqN.bam>" << std::endl;
        std::cout << visible_options << std::endl;
        std::cout << std::endl;
        std::cout << "Notes:" << std::endl;
        std::cout << "  * Reads are counted by start position" << std::endl;
        std::cout << "  * One cell per BAM file, inclusing SM tag in header" << std::endl;
        std::cout << "  * For paired-end data, only read 1 is counted" << std::endl;
        return 1;
    }


    /////////////////////////////////////////////////////////// global variables
    /* leave one BAM header open to get chrom names & lengths */
    bam_hdr_t* hdr = NULL;

    /* regarding each cell */
    std::vector<CellInfo>       cells(conf.f_in.size());
    std::vector<TGenomeCounts>  counts(conf.f_in.size());

    /* regarding each sample */
    std::unordered_map<std::string, SampleInfo> samples;

    /* regarding bins */
    std::vector<Interval>       bins;
    std::vector<int32_t>        chrom_map;
    std::vector<unsigned>       good_bins;
    std::vector<int32_t>        good_map;
    ////////////////////////////////////////////////////////////////////////////



    //
    // Chapter: Binning & counting
    // ===========================
    //

    // Read sample names from headers.
    // Keep one header throughout the program.
    std::cout << "Exploring SAM headers..." << std::endl;
    for(int i = 0; i < conf.f_in.size(); ++i)
    {
        cells[i].id = i;
        samFile* samfile = sam_open(conf.f_in[i].string().c_str(), "r");
        if (samfile == NULL) {
            std::cerr << "[Error] Fail to open file " << conf.f_in[0].string() << std::endl;
            return 1;
        }
        hdr = sam_hdr_read(samfile);
        if (!get_SM_tag(hdr->text, cells[i].sample_name)) {
            std::cerr << "[Error] Each BAM file has to have exactly one SM tag." << std::endl << std::endl;
            goto print_usage_and_exit;
        }
        sam_close(samfile);
    }


    // Bin the genome
    unsigned median_binsize;
    chrom_map = std::vector<int32_t>(hdr->n_targets, -1);
    if (vm.count("bins"))
    {
        if (!read_dynamic_bins(bins, chrom_map, conf.f_bins.string().c_str(), hdr))
            return 1;
        TMedianAccumulator<unsigned> med_acc;
        for (Interval const & b : bins)
            med_acc(b.end - b.start);
        median_binsize = boost::accumulators::median(med_acc);
        std::cout << "Reading " << bins.size() << " variable-width bins with median bin size of " << round(median_binsize/1000) << "kb" << std::endl;
    }
    else
    {
        std::vector<Interval> exclude;
        if (vm.count("exclude")) {
            read_exclude_file(conf.f_excl.string(), hdr, exclude);
            sort(exclude.begin(), exclude.end(), interval_comp);
        }
        std::cout << "Creating " << round(conf.window/1000) << "kb bins with " << exclude.size() << " excluded regions" << std::endl;
        create_fixed_bins(bins, chrom_map, conf.window, exclude, hdr);
        median_binsize = conf.window;
    }
    // add last element for easy calculation of number of bins
    chrom_map.push_back((int32_t)bins.size());


    // Count in bins. If A bam file cannot be read, the cell is ignored and
    //     the respective entry in `counts` and `cells` will be erased.
    std::cout << "Reading " << conf.f_in.size() <<  " BAM files...";
    boost::progress_display show_progress1(conf.f_in.size());
    for(unsigned i = 0, i_f = 0; i_f < conf.f_in.size(); ++i, ++i_f)
    {
        if (!count_sorted_reads(conf.f_in[i_f].string(), bins, chrom_map, hdr, conf.minMapQual, counts[i], cells[i])) {
            std::cerr << "[Warning] Ignoring cell " << conf.f_in[i_f].string() << std::endl;
            counts.erase(counts.begin()+i);
            cells.erase(cells.begin()+i);
            --i;
        }
        ++show_progress1;
    }

    // median count per sample
    for(unsigned i = 0; i < counts.size(); ++i) {
        TMedianAccumulator<unsigned int> med_acc;
        for (Counter const & count_bin : counts[i])
            med_acc(count_bin.watson_count + count_bin.crick_count);
        cells[i].median_bin_count = boost::accumulators::median(med_acc);
    }


    // Print cell information:
    if (vm.count("info")) {
        std::cout << "[Write] Cell summary: " << conf.f_info.string() << std::endl;
        std::ofstream out(conf.f_info.string());
        if (out.is_open()) {
            out << "# medbin:  Median total count (w+c) per bin" << std::endl;
            out << "# mapped:  Total number of reads seen" << std::endl;
            out << "# suppl:   Supplementary, secondary or QC-failed reads (filtered out)" << std::endl;
            out << "# dupl:    Reads filtered out as PCR duplicates" << std::endl;
            out << "# mapq:    Reads filtered out due to low mapping quality" << std::endl;
            out << "# read2:   Reads filtered out as 2nd read of pair" << std::endl;
            out << "# good:    Reads used for counting." << std::endl;
            out << "sample\tcell\tmedbin\tmapped\tsuppl\tdupl\tmapq\tread2\tgood" << std::endl;

            // do not sort "cells" itselft, so cells == counts == conf.f_in
            std::vector<CellInfo> cells2 = cells; // copy
            sort(cells2.begin(), cells2.end(), [] (CellInfo const & a, CellInfo const & b) {if (a.sample_name==b.sample_name) { return a.id < b.id;} else {return a.sample_name < b.sample_name;} } );

            for (CellInfo const & cell : cells2) {
                out << cell.sample_name << "\t";
                out << conf.f_in[cell.id].stem().string() << "\t";
                out << cell.median_bin_count << "\t";
                out << cell.n_mapped << "\t";
                out << cell.n_supplementary << "\t";
                out << cell.n_pcr_dups << "\t";
                out << cell.n_low_mapq << "\t";
                out << cell.n_read2s << "\t";
                out << cell.n_counted << std::endl;
            }
        } else {
            std::cerr << "Cannot write to " << conf.f_info.string() << std::endl;
        }
    }




    //
    // Chapter: Remove bad bins & estimate BN parameter p
    // ==================================================
    //
    {
        // Median-Normalized counts
        std::vector<std::vector<std::tuple<float,float>>> norm_counts(counts.size(), std::vector<std::tuple<float,float>>(bins.size()));
        for(unsigned i = 0; i < counts.size(); ++i)
            for (unsigned bin = 0; bin < bins.size(); ++bin)
                norm_counts[i][bin] = std::make_tuple(counts[i][bin].watson_count/(float)cells[i].median_bin_count,
                                                      counts[i][bin].crick_count/(float)cells[i].median_bin_count);

        // mean + variance per bin
        std::vector<float> bin_means(bins.size());
        std::vector<float> bin_variances(bins.size());
        for (unsigned bin = 0; bin < bins.size(); ++bin) {
            TMeanVarAccumulator<float> meanvar_acc;
            for (unsigned i = 0; i < counts.size(); ++i)
                meanvar_acc(std::get<0>(norm_counts[i][bin]) + std::get<1>(norm_counts[i][bin]));
            bin_means[bin]     = boost::accumulators::mean(meanvar_acc);
            bin_variances[bin] = boost::accumulators::variance(meanvar_acc);
        }

        // finding good bins
        TMeanVarAccumulator<float> meanvar_acc;
        std::vector<char> bad_bins;
        for (unsigned bin = 0; bin < bins.size(); ++bin)
            meanvar_acc(bin_means[bin]);
        float my_mean = boost::accumulators::mean(meanvar_acc);
        float my_sd   = std::sqrt(boost::accumulators::variance(meanvar_acc));
        std::cout << "Mean mean bin count is " << my_mean << std::endl;
        std::cout << "mean bin count SD is "   << my_sd << std::endl;
        for (unsigned bin = 0; bin < bins.size(); ++bin)
            if (bin_means[bin] > 0.01 && bin_means[bin] < my_mean + 3*my_sd)
                good_bins.push_back(bin);
            else
                bad_bins.push_back(bin_means[bin] <= 0.01 ? 'l' : 'h');
        std::cout << "Filtering " << bins.size() - good_bins.size() << " bins." << std::endl;


        // Write removed bins to bed file
        if (vm.count("removed_bins")) {
            std::cout << "[Write] removed bins: " << conf.f_removed_bins.string() << std::endl;
            std::ofstream out(conf.f_removed_bins.string());
            if (out.is_open()) {
                auto goodit = good_bins.begin();
                auto badit  = bad_bins.begin();
                for (unsigned bin = 0; bin < bins.size(); ++bin) {
                    if(goodit == good_bins.end() || bin < *goodit) {
                        out << hdr->target_name[bins[bin].chr] << "\t" << bins[bin].start << "\t" << bins[bin].end << "\t" << *badit++ << std::endl;
                    } else {
                        if (goodit != good_bins.end()) goodit++;
                    }
                }
                assert(badit == bad_bins.end());
            } else {
                std::cerr << "Cannot write to " << conf.f_removed_bins.string() << std::endl;
            }
        }


        // build chrom_map for good bins
        good_map = std::vector<int32_t>(hdr->n_targets, -1);
        int32_t pos = 0;
        for (int32_t chr = 0; chr < hdr->n_targets; ++chr) {
            while (pos < good_bins.size() && bins[good_bins[pos]].chr < chr)
                ++pos;
            // now goodit is either at first occurence of chr, or at the end.
            if (pos >= good_bins.size()) good_map[chr] = (int32_t)good_bins.size();
            else good_map[chr] = pos;
        }
        // add last element for easy calculation of number of bins
        good_map.push_back((int32_t)good_bins.size());


        // calculate cell means and cell variances, grouped by sample (not cell)
        for (unsigned i = 0; i < counts.size(); ++i) {

            // Get mean and var for this cell, but only from good bins!
            TMeanVarAccumulator<float> acc;
            for (unsigned bini = 0; bini < good_bins.size(); ++bini) {
                acc(counts[i][good_bins[bini]].crick_count + counts[i][good_bins[bini]].watson_count);
            }
            // emplace finds key if existing and returns (it,false);
            // otherwise it inserts (key,value) and returns (it,true).
            auto it = samples.begin();
            std::tie(it, std::ignore) = samples.emplace(cells[i].sample_name, SampleInfo());
            (it->second).means.push_back(boost::accumulators::mean(acc));
            (it->second).vars.push_back(boost::accumulators::variance(acc));
        }

        // Estimation of parameter p per sample (should work even with one cell only)
        for (auto it = samples.begin(); it != samples.end(); ++it) {
            SampleInfo & s = it->second;
            s.p = std::inner_product(s.means.begin(), s.means.end(), s.means.begin(), 0.0f) \
                / std::inner_product(s.means.begin(), s.means.end(), s.vars.begin(), 0.0f);
        }

        // Write sample information to file
        if (vm.count("sample_info")) {
            std::cout << "[Write] sample information: " << conf.f_sample_info.string() << std::endl;
            std::ofstream out(conf.f_sample_info.string());
            if (out.is_open()) {
                out << "sample\tcells\tp\tmeans\tvars" << std::endl;
                for (auto it = samples.begin(); it != samples.end(); ++it) {
                    SampleInfo const & s = it->second;
                    out << it->first << "\t" << s.means.size() << "\t" << s.p << "\t" << s.means[0];
                    for (unsigned k=1; k<s.means.size(); ++k) out << "," << s.means[k];
                    out << "\t" << s.vars[0];
                    for (unsigned k=1; k<s.vars.size(); ++k) out << "," << s.vars[k];
                    out << std::endl;
                }
            } else {
                std::cerr << "Cannot write to " << conf.f_sample_info.string() << std::endl;
            }
        }
    }



    //
    // Chapter: Run HMM
    // ================
    //

    // Set up and run HMM:
    hmm::HMM<unsigned, hmm::MultiVariate<hmm::NegativeBinomial> > hmm({"CC", "WC", "WW"});
    hmm.set_initials({0.3333, 0.3333, 0.3333});

    // Estimate transition probabilities in the order of 10 SCEs per cell
    double p_trans = 10.0f / bins.size();
    hmm.set_transitions({1-2*p_trans, p_trans,     p_trans,   \
                         p_trans,     1-2*p_trans, p_trans,   \
                         p_trans,     p_trans,     1-2*p_trans});


    for (unsigned i=0; i<counts.size(); ++i)
    {
        // set NB(n,p) parameters according to `p` of sample and mean of cell.
        double p = samples[cells[i].sample_name].p;
        double n = (double)cells[i].median_bin_count / 2 * p / (1-p);

        // todo: adjust mean in zero bins !!
        double z = 0.5; // mean in zero bins

        hmm.set_emissions( {\
            hmm::MultiVariate<hmm::NegativeBinomial>({hmm::NegativeBinomial(p,2*n), hmm::NegativeBinomial(p,  z)}), // CC
            hmm::MultiVariate<hmm::NegativeBinomial>({hmm::NegativeBinomial(p,  n), hmm::NegativeBinomial(p,  n)}), // WC
            hmm::MultiVariate<hmm::NegativeBinomial>({hmm::NegativeBinomial(p,  z), hmm::NegativeBinomial(p,2*n)})  // WW
        });
        run_HMM(hmm, counts[i], good_bins, good_map);
    }




    std::cout << "[Write] count table: " << conf.f_out.string() << std::endl;
    std::ofstream out(conf.f_out.string());
    if (out.is_open()) {
        out << "chrom\tstart\tend\tsample\tcell\tc\tw\tclass" << std::endl;
        for(unsigned i = 0; i < counts.size(); ++i) {
            for (unsigned bin = 0; bin < counts[i].size(); ++bin) {
                Counter & cc = counts[i][bin];
                out << hdr->target_name[bins[bin].chr];
                out << "\t" << bins[bin].start << "\t" << bins[bin].end;
                out << "\t" << cells[i].sample_name;
                out << "\t" << conf.f_in[i].stem().string();
                out << "\t" << cc.crick_count;
                out << "\t" << cc.watson_count;
                out << "\t" << cc.get_label();
                out << std::endl;
            }
        }
        out.close();
    } else {
        std::cerr << "[Error] Cannot open file: " << conf.f_out.string() << std::endl;
        return 2;
    }
    return(0);

}
