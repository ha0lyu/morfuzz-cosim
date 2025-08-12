#include <stdio.h>
#include <stdlib.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "cj.h"

using namespace std;
namespace fs = std::filesystem;


inline std::string quiet(const std::string &cmd);
void read_all_files(const string &directory, vector<string> &files);
void save_bin(const string &filename, std::map<uint64_t, uint32_t> &pc2inst);
void merge_bin(const string &objcopy, const string &elf, const string &mutated_bin);
void instruction_level_mutator(config_t cfg, std::map<uint64_t, uint32_t>& pc2inst, int instret);
vector<string> gen_output_file_name(const string &filename, const string &outputdir, int num);


int main(int argc, const char *argv[]) {
    if(argc < 4){
        fprintf(stderr, "Usage: %s elf output_Dir mutate_time [instret]\n", argv[0]);
    }
    const string elf_dir = argv[1];
    const string output = argv[2];
    const int mutation_times =  atoi(argv[3]);
    const int instret = (argc == 5) ? atoi(argv[4]) : 20000;


    string objcopy = "riscv64-unknown-elf-objcopy";     // modify it when it's `riscv64-linux-elf-objcopy`
    string test_obj = objcopy + " -h";
    if(system(quiet(test_obj).c_str())){
        fprintf(stderr, "Check %s first\n", objcopy.c_str());
        exit(0);
    }

    vector<string>elfs;
    read_all_files(elf_dir, elfs);
    if(elfs.empty()){
        fprintf(stderr, "[ERROR] At least one testcase is required!");
        exit(0);
    }
    // foreach elf mutate times and save and merge;
    for(auto& elf : elfs){
        config_t cfg;
        cfg.verbose = false;
        cfg.isa = "rv64gc_zicsr_zicntr";
        cfg.boot_addr = 0x80000000;
        cfg.elffiles = std::vector<std::string>{ elf };     // elf
        cfg.mem_layout = std::vector<mem_cfg_t>{
            mem_cfg_t(0x80000000UL, 0x80000000UL),
        };
        cfg.logfile = fopen("/dev/null", "wt");
        cosim_cj_t *simulator= new cosim_cj_t(cfg);
        vector<string>bins = gen_output_file_name(elf, output, mutation_times);
        for(auto& bin : bins){
            std::map<uint64_t, uint32_t> pc2inst;
            cout << "[INFO] Processing: " << bin << endl;
            masker_inst_t::fence_mutation();    // clear history for each time.
            simulator->cosim_commit_stage(0, 0, 0, false);
            processor_t *core = simulator->get_core(0);
            // core->reset();           // <--- Bug !!!! DO NOT reset() !
            int instcount = instret;

            while(instcount--){
                uint32_t pc = core->get_state()->last_pc;
                uint32_t insn = core->get_state()->last_insn;
                uint64_t mutated_insn = simulator->cosim_randomizer_insn(insn, pc);
                pc2inst[pc] = mutated_insn;
                core->step(1);
                // if(insn != mutated_insn){
                //     printf("PC: 0x%016lx, Insn: 0x%08x, Mutated Insn: 0x%08lx\n", pc, insn, mutated_insn);
                // }
                // else{
                //     printf("PC: 0x%016lx, Insn: 0x%08x\n", pc, insn);
                // }
            }
            save_bin(bin, pc2inst);
            merge_bin(objcopy, elf, bin);
            cout << "[INFO] Processed: " << bin << endl;            
        }
    }
}


inline std::string quiet(const std::string &cmd){
    return cmd + " > /dev/null 2>&1";
}

/*
    NOTE: All elf files should be in same dir and only elfs.
*/
void read_all_files(const string &directory, vector<string> &files){
    files.clear();
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.push_back(fs::absolute(entry.path()).string());
        }
    }
}

void save_bin(const string &filename, std::map<uint64_t, uint32_t> &pc2inst)
{
    if(pc2inst.empty()){
        fprintf(stderr, "No instructions.\n");
        return;
    }
    std::ofstream outfile(filename, std::ios::binary);
    cout << "[INFO] Saving: " << filename << endl;
    
    if(outfile.is_open() == false){
        fprintf(stderr, "Failed to open file %s for writing.\n", filename.c_str());
        return;
    }

    uint64_t base_pc = 0x80000000;
    uint64_t last_pc = base_pc;

    for (auto &[addr, inst] : pc2inst) {
        // printf("addr = 0x%016lx    pc = 0x%016lx,    inst = 0x%08x\n", addr, last_pc, inst);  // debug
        
        if (addr > last_pc) {
            uint64_t file_offset = addr - base_pc;
            // fill the gap.
            outfile.seekp(file_offset, std::ios::beg);      
        }
        if ((inst & 0x3) != 0x3) {
            uint16_t ci = static_cast<uint16_t>(inst & 0xFFFF);
            outfile.write(reinterpret_cast<const char*>(&ci), 2);
            last_pc = addr + 2;
        } else {
            uint32_t rvi = static_cast<uint32_t>(inst);
            outfile.write(reinterpret_cast<const char*>(&rvi), 4);
            last_pc = addr + 4;
        }
    }

    cout << "[INFO] Saved: " << filename << endl;
    outfile.close();
}


/*
    1. run objcopy to convert elf to bin
    2. merge mutated bin into elf.bin
    3. replace mutated bin
*/
void merge_bin(const string &objcopy, const string &elf, const string &mutated_bin){
    string cmd = objcopy + " -O binary " + elf + " /tmp/elf.bin";
    if(system(quiet(cmd).c_str())){
        fprintf(stderr, "[ERROR] Run cmd `%s` error.\n", cmd.c_str());
        exit(-1);
    }
    cmd = "dd if=" + mutated_bin + " of=/tmp/elf.bin bs=1 conv=notrunc";
    if(system(quiet(cmd).c_str())){
        fprintf(stderr, "[ERROR] Run cmd `%s` error.\n", cmd.c_str());
        exit(-1);
    }
    cmd = "mv /tmp/elf.bin " + mutated_bin;     // replace the mutated bin file
    if(system(quiet(cmd).c_str())){
        fprintf(stderr, "[ERROR] Run cmd `%s` error.\n", cmd.c_str());
        exit(-1);
    }
} 

void instruction_level_mutator(config_t cfg, std::map<uint64_t, uint32_t>& pc2inst, int instret){
    cosim_cj_t *simulator= new cosim_cj_t(cfg);
    masker_inst_t::fence_mutation();    // clear history for each time.
    simulator->cosim_commit_stage(0, 0, 0, false);
    processor_t *core = simulator->get_core(0);

    while(instret--){
        uint32_t pc = core->get_state()->last_pc;
        uint32_t insn = core->get_state()->last_insn;
        uint64_t mutated_insn = simulator->cosim_randomizer_insn(insn, pc);
        pc2inst[pc] = mutated_insn;
        core->step(1);
        // if(insn != mutated_insn){
        //     printf("PC: 0x%016lx, Insn: 0x%08x, Mutated Insn: 0x%08lx\n", pc, insn, mutated_insn);
        // }
        // else{
        //     printf("PC: 0x%016lx, Insn: 0x%08x\n", pc, insn);
        // }
    }
}

vector<string> gen_output_file_name(const string &filename, const string &outputdir, int num)
{
    fs::path inputPath(filename);
    fs::path outputDir(outputdir);
    string base_filename = inputPath.stem().string();
    string extension, output_filename;
    vector<string> output_filenames;
    extension = ".bin";
    for(int i = 0; i < num; ++i){
        if(i == 0){     // Keeping the seed name unchanged
            output_filename = base_filename + "_inst_" + extension;
        }
        else{
            output_filename = base_filename + "_inst_" + to_string(i) + "M" + extension;
        }
        fs::path full_output_path = outputDir / output_filename;
        output_filenames.push_back(full_output_path.string());
    }
    return output_filenames;    
}
