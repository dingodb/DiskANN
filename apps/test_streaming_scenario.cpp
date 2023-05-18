// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <index.h>
#include <numeric>
#include <omp.h>
#include <string.h>
#include <time.h>
#include <timer.h>
#include <boost/program_options.hpp>
#include <future>

#include "utils.h"
#include "filter_utils.h"

#ifndef _WINDOWS
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "memory_mapper.h"

namespace po = boost::program_options;

// load_aligned_bin modified to read pieces of the file, but using ifstream
// instead of cached_ifstream.
template <typename T>
inline void load_aligned_bin_part(const std::string &bin_file, T *data, size_t offset_points, size_t points_to_read)
{
    std::ifstream reader;
    reader.exceptions(std::ios::failbit | std::ios::badbit);
    reader.open(bin_file, std::ios::binary | std::ios::ate);
    size_t actual_file_size = reader.tellg();
    reader.seekg(0, std::ios::beg);

    int npts_i32, dim_i32;
    reader.read((char *)&npts_i32, sizeof(int));
    reader.read((char *)&dim_i32, sizeof(int));
    size_t npts = (uint32_t)npts_i32;
    size_t dim = (uint32_t)dim_i32;

    size_t expected_actual_file_size = npts * dim * sizeof(T) + 2 * sizeof(uint32_t);
    if (actual_file_size != expected_actual_file_size)
    {
        std::stringstream stream;
        stream << "Error. File size mismatch. Actual size is " << actual_file_size << " while expected size is  "
               << expected_actual_file_size << " npts = " << npts << " dim = " << dim << " size of <T>= " << sizeof(T)
               << std::endl;
        std::cout << stream.str();
        throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__, __LINE__);
    }

    if (offset_points + points_to_read > npts)
    {
        std::stringstream stream;
        stream << "Error. Not enough points in file. Requested " << offset_points << "  offset and " << points_to_read
               << " points, but have only " << npts << " points" << std::endl;
        std::cout << stream.str();
        throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__, __LINE__);
    }

    reader.seekg(2 * sizeof(uint32_t) + offset_points * dim * sizeof(T));

    const size_t rounded_dim = ROUND_UP(dim, 8);

    for (size_t i = 0; i < points_to_read; i++)
    {
        reader.read((char *)(data + i * rounded_dim), dim * sizeof(T));
        memset(data + i * rounded_dim + dim, 0, (rounded_dim - dim) * sizeof(T));
    }
    reader.close();
}

std::string get_save_filename(const std::string &save_path, size_t active_window, size_t consolidate_interval,
                              size_t max_points_to_insert)
{
    std::string final_path = save_path;
    final_path += "act" + std::to_string(active_window) + "-";
    final_path += "cons" + std::to_string(consolidate_interval) + "-";
    final_path += "max" + std::to_string(max_points_to_insert);
    return final_path;
}

template <typename T, typename TagT, typename LabelT>
void insert_next_batch(diskann::Index<T, TagT, LabelT> &index, size_t start, size_t end, size_t insert_threads, T *data,
                       size_t aligned_dim, std::vector<std::vector<LabelT>> &labels)
{
    try
    {
        diskann::Timer insert_timer;
        std::cout << std::endl << "Inserting from " << start << " to " << end << std::endl;

        size_t num_failed = 0;
#pragma omp parallel for num_threads((int32_t)insert_threads) schedule(dynamic) reduction(+ : num_failed)
        for (int64_t j = start; j < (int64_t)end; j++)
        {
            int insert_result = -1;
            if (labels.size() > 0)
            {
                insert_result =
                    index.insert_point(&data[(j - start) * aligned_dim], 1 + static_cast<TagT>(j), labels[j - start]);
            }
            else
            {
                insert_result = index.insert_point(&data[(j - start) * aligned_dim], 1 + static_cast<TagT>(j));
            }

            if (insert_result != 0)
            {
                std::cerr << "Insert failed " << j << std::endl;
                num_failed++;
            }
        }
        const double elapsedSeconds = insert_timer.elapsed() / 1000000.0;
        std::cout << "Insertion time " << elapsedSeconds << " seconds (" << (end - start) / elapsedSeconds
                  << " points/second overall, " << (end - start) / elapsedSeconds / insert_threads << " per thread)"
                  << std::endl;
        if (num_failed > 0)
            std::cout << num_failed << " of " << end - start << "inserts failed" << std::endl;
    }
    catch (std::system_error &e)
    {
        std::cout << "Exiting after catching exception in insertion task: " << e.what() << std::endl;
    }
}

template <typename T, typename TagT, typename LabelT>
void delete_and_consolidate(diskann::Index<T, TagT, LabelT> &index, const diskann::IndexWriteParameters &delete_params,
                            size_t start, size_t end)
{
    try
    {
        std::cout << std::endl << "Lazy deleting points " << start << " to " << end << "... ";
        for (size_t i = start; i < end; ++i)
            index.lazy_delete(static_cast<TagT>(1 + i));
        std::cout << "lazy delete done." << std::endl;

        auto report = index.consolidate_deletes(delete_params);
        while (report._status != diskann::consolidation_report::status_code::SUCCESS)
        {
            int wait_time = 5;
            if (report._status == diskann::consolidation_report::status_code::LOCK_FAIL)
            {
                diskann::cerr << "Unable to acquire consolidate delete lock after "
                              << "deleting points " << start << " to " << end << ". Will retry in " << wait_time
                              << "seconds." << std::endl;
            }
            else if (report._status == diskann::consolidation_report::status_code::INCONSISTENT_COUNT_ERROR)
            {
                diskann::cerr << "Inconsistent counts in data structure. "
                              << "Will retry in " << wait_time << "seconds." << std::endl;
            }
            else
            {
                std::cerr << "Exiting after unknown error in consolidate delete" << std::endl;
                exit(-1);
            }
            std::this_thread::sleep_for(std::chrono::seconds(wait_time));
            report = index.consolidate_deletes(delete_params);
        }
        auto points_processed = report._active_points + report._slots_released;
        auto deletion_rate = points_processed / report._time;
        std::cout << "#active points: " << report._active_points << std::endl
                  << "max points: " << report._max_points << std::endl
                  << "empty slots: " << report._empty_slots << std::endl
                  << "deletes processed: " << report._slots_released << std::endl
                  << "latest delete size: " << report._delete_set_size << std::endl
                  << "Deletion rate: " << deletion_rate << "/sec   "
                  << "Deletion rate: " << deletion_rate / delete_params.num_threads << "/thread/sec   " << std::endl;
    }
    catch (std::system_error &e)
    {
        std::cerr << "Exiting after catching exception in deletion task: " << e.what() << std::endl;
        exit(-1);
    }
}

template <typename T, typename TagT = uint32_t, typename LabelT = uint32_t>
void build_incremental_index(const std::string &data_path, const uint32_t L, const uint32_t R, const float alpha,
                             const uint32_t insert_threads, const uint32_t consolidate_threads,
                             size_t max_points_to_insert, size_t active_window, size_t consolidate_interval,
                             const float start_point_norm, uint32_t num_start_pts, const std::string &save_path,
                             const std::string &label_file, const std::string &universal_label, const uint32_t Lf)
{
    const uint32_t C = 500;
    const bool saturate_graph = false;
    bool has_labels = label_file != "";
    std::string labels_file_to_use = save_path + "_label_formatted.txt";
    std::string mem_labels_int_map_file = save_path + "_labels_map.txt";

    diskann::IndexWriteParameters params = diskann::IndexWriteParametersBuilder(L, R)
                                               .with_max_occlusion_size(C)
                                               .with_alpha(alpha)
                                               .with_saturate_graph(saturate_graph)
                                               .with_num_threads(insert_threads)
                                               .with_num_frozen_points(num_start_pts)
                                               .with_labels(has_labels)
                                               .with_filter_list_size(Lf)
                                               .build();

    diskann::IndexWriteParameters delete_params = diskann::IndexWriteParametersBuilder(L, R)
                                                      .with_max_occlusion_size(C)
                                                      .with_alpha(alpha)
                                                      .with_saturate_graph(saturate_graph)
                                                      .with_num_threads(consolidate_threads)
                                                      .with_labels(has_labels)
                                                      .with_filter_list_size(Lf)
                                                      .build();

    size_t dim, aligned_dim;
    size_t num_points;

    std::vector<std::vector<LabelT>> labels;

    if (has_labels)
    {
        convert_labels_string_to_int(label_file, labels_file_to_use, mem_labels_int_map_file, universal_label);
        auto parse_result = diskann::parse_formatted_label_file<LabelT>(labels_file_to_use);
        labels = std::get<0>(parse_result);
    }

    diskann::get_bin_metadata(data_path, num_points, dim);
    diskann::cout << "metadata: file " << data_path << " has " << num_points << " points in " << dim << " dims"
                  << std::endl;
    aligned_dim = ROUND_UP(dim, 8);

    if (max_points_to_insert == 0)
    {
        max_points_to_insert = num_points;
    }

    if (num_points < max_points_to_insert)
        throw diskann::ANNException(std::string("num_points(") + std::to_string(num_points) +
                                        ") < max_points_to_insert(" + std::to_string(max_points_to_insert) + ")",
                                    -1, __FUNCSIG__, __FILE__, __LINE__);

    if (max_points_to_insert < active_window + consolidate_interval)
        throw diskann::ANNException("ERROR: max_points_to_insert < "
                                    "active_window + consolidate_interval",
                                    -1, __FUNCSIG__, __FILE__, __LINE__);

    if (consolidate_interval < max_points_to_insert / 1000)
        throw diskann::ANNException("ERROR: consolidate_interval is too small", -1, __FUNCSIG__, __FILE__, __LINE__);

    const bool enable_tags = true;

    diskann::Index<T, TagT, LabelT> index(diskann::L2, dim, active_window + 4 * consolidate_interval, true, params, L,
                                          insert_threads, enable_tags, true);

    if (universal_label != "")
    {
        LabelT unv_label_as_num = 0;
        index.set_universal_label(unv_label_as_num);
    }

    index.set_start_points_at_random(static_cast<T>(start_point_norm));

    // TODO: Is this necessary?
    if (!has_labels)
    {
        index.enable_delete();
    }

    T *data = nullptr;
    diskann::alloc_aligned((void **)&data, std::max(consolidate_interval, active_window) * aligned_dim * sizeof(T),
                           8 * sizeof(T));

    std::vector<TagT> tags(max_points_to_insert);
    std::iota(tags.begin(), tags.end(), static_cast<TagT>(0));

    diskann::Timer timer;

    std::vector<std::future<void>> delete_tasks;

    auto insert_task = std::async(std::launch::async, [&]() {
        load_aligned_bin_part(data_path, data, 0, active_window);
        insert_next_batch(index, 0, active_window, params.num_threads, data, aligned_dim, labels);
    });
    insert_task.wait();

    for (size_t start = active_window; start + consolidate_interval <= max_points_to_insert;
         start += consolidate_interval)
    {
        auto end = std::min(start + consolidate_interval, max_points_to_insert);
        auto insert_task = std::async(std::launch::async, [&]() {
            load_aligned_bin_part(data_path, data, start, end - start);
            insert_next_batch(index, start, end, params.num_threads, data, aligned_dim, labels);
        });
        insert_task.wait();

        if (!has_labels)
        {
            if (delete_tasks.size() > 0)
                delete_tasks[delete_tasks.size() - 1].wait();
            if (start >= active_window + consolidate_interval)
            {
                auto start_del = start - active_window - consolidate_interval;
                auto end_del = start - active_window;

                delete_tasks.emplace_back(std::async(
                    std::launch::async, [&]() { delete_and_consolidate(index, delete_params, start_del, end_del); }));
            }
        }
        else
        {
            std::cout << "Warning: Deleting points is not yet supported for labeled data";
        }
    }
    if (delete_tasks.size() > 0)
        delete_tasks[delete_tasks.size() - 1].wait();

    std::cout << "Time Elapsed " << timer.elapsed() / 1000 << "ms\n";
    const auto save_path_inc =
        get_save_filename(save_path + ".after-streaming-", active_window, consolidate_interval, max_points_to_insert);
    index.save(save_path_inc.c_str(), true);

    diskann::aligned_free(data);
}

int main(int argc, char **argv)
{
    std::string data_type, dist_fn, data_path, index_path_prefix, label_file, universal_label, label_type;
    uint32_t insert_threads, consolidate_threads, R, L, num_start_pts, Lf;
    float alpha, start_point_norm;
    size_t max_points_to_insert, active_window, consolidate_interval;

    po::options_description desc{"Arguments"};
    try
    {
        desc.add_options()("help,h", "Print information on arguments");
        desc.add_options()("data_type", po::value<std::string>(&data_type)->required(), "data type <int8/uint8/float>");
        desc.add_options()("dist_fn", po::value<std::string>(&dist_fn)->required(), "distance function <l2/mips>");
        desc.add_options()("data_path", po::value<std::string>(&data_path)->required(),
                           "Input data file in bin format");
        desc.add_options()("index_path_prefix", po::value<std::string>(&index_path_prefix)->required(),
                           "Path prefix for saving index file components");
        desc.add_options()("max_degree,R", po::value<uint32_t>(&R)->default_value(64), "Maximum graph degree");
        desc.add_options()("Lbuild,L", po::value<uint32_t>(&L)->default_value(100),
                           "Build complexity, higher value results in better graphs");
        desc.add_options()("alpha", po::value<float>(&alpha)->default_value(1.2f),
                           "alpha controls density and diameter of graph, set "
                           "1 for sparse graph, "
                           "1.2 or 1.4 for denser graphs with lower diameter");
        desc.add_options()("insert_threads",
                           po::value<uint32_t>(&insert_threads)->default_value(omp_get_num_procs() / 2),
                           "Number of threads used for inserting into the index (defaults to "
                           "omp_get_num_procs()/2)");
        desc.add_options()("consolidate_threads",
                           po::value<uint32_t>(&consolidate_threads)->default_value(omp_get_num_procs() / 2),
                           "Number of threads used for consolidating deletes to "
                           "the index (defaults to omp_get_num_procs()/2)");

        desc.add_options()("max_points_to_insert", po::value<uint64_t>(&max_points_to_insert)->default_value(0),
                           "The number of points from the file that the program streams "
                           "over ");
        desc.add_options()("active_window", po::value<uint64_t>(&active_window)->required(),
                           "Program maintains an index over an active window of "
                           "this size that slides through the data");
        desc.add_options()("consolidate_interval", po::value<uint64_t>(&consolidate_interval)->required(),
                           "The program simultaneously adds this number of points to the "
                           "right of "
                           "the window while deleting the same number from the left");
        desc.add_options()("start_point_norm", po::value<float>(&start_point_norm)->required(),
                           "Set the start point to a random point on a sphere of this radius");
        desc.add_options()(
            "num_start_points",
            po::value<uint32_t>(&num_start_pts)->default_value(diskann::defaults::NUM_FROZEN_POINTS_DYNAMIC),
            "Set the number of random start (frozen) points to use when "
            "inserting and searching");

        // Args for building index of labeled data
        desc.add_options()("label_file", po::value<std::string>(&label_file)->default_value(""),
                           "Input label file in txt format for Filtered Index search. "
                           "The file should contain comma separated filters for each node "
                           "with each line corresponding to a graph node");
        desc.add_options()("universal_label", po::value<std::string>(&universal_label)->default_value(""),
                           "Universal label, if using it, only in conjunction with labels_file");
        desc.add_options()("FilteredLbuild,Lf", po::value<uint32_t>(&Lf)->default_value(0),
                           "Build complexity for filtered points, higher value "
                           "results in better graphs");
        desc.add_options()("label_type", po::value<std::string>(&label_type)->default_value("uint"),
                           "Storage type of Labels <uint/ushort>, default value is uint which "
                           "will consume memory 4 bytes per filter");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << desc;
            return 0;
        }
        po::notify(vm);
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return -1;
    }

    // Validate arguments
    if (start_point_norm == 0)
    {
        std::cout << "When beginning_index_size is 0, use a start point with "
                     "appropriate norm"
                  << std::endl;
        return -1;
    }

    if (label_type != std::string("ushort") && label_type != std::string("uint"))
    {
        std::cerr << "Invalid label type. Supported types are uint and ushort" << std::endl;
        return -1;
    }

    if (data_type != std::string("int8") && data_type != std::string("uint8") && data_type != std::string("float"))
    {
        std::cerr << "Invalid data type. Supported types are int8, uint8 and float" << std::endl;
        return -1;
    }

    // TODO: Are additional distance functions supported?
    if (dist_fn != std::string("l2") && dist_fn != std::string("mips"))
    {
        std::cerr << "Invalid distance function. Supported functions are l2 and mips" << std::endl;
        return -1;
    }

    try
    {
        if (data_type == std::string("uint8"))
        {
            if (label_type == std::string("ushort"))
            {
                build_incremental_index<uint8_t, uint32_t, uint16_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
            else if (label_type == std::string("uint"))
            {
                build_incremental_index<uint8_t, uint32_t, uint32_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
        }
        else if (data_type == std::string("int8"))
        {
            if (label_type == std::string("ushort"))
            {
                build_incremental_index<int8_t, uint32_t, uint16_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
            else if (label_type == std::string("uint"))
            {
                build_incremental_index<int8_t, uint32_t, uint32_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
        }
        else if (data_type == std::string("float"))
        {
            if (label_type == std::string("ushort"))
            {
                build_incremental_index<float, uint32_t, uint16_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
            else if (label_type == std::string("uint"))
            {
                build_incremental_index<float, uint32_t, uint32_t>(
                    data_path, L, R, alpha, insert_threads, consolidate_threads, max_points_to_insert, active_window,
                    consolidate_interval, start_point_norm, num_start_pts, index_path_prefix, label_file,
                    universal_label, Lf);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Caught exception: " << e.what() << std::endl;
        exit(-1);
    }
    catch (...)
    {
        std::cerr << "Caught unknown exception" << std::endl;
        exit(-1);
    }

    return 0;
}