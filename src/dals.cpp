/**
 * @file dals.cpp
 * @brief
 * @author Nathan Zhou
 * @date 2019-01-20
 * @bug In some cases, the depth of the circuit stays the same for multiple rounds.
 */

#include <iostream>
#include <algorithm>
#include <bitset>
#include <iomanip>

// resolve conflict between cpu timers and original timers (deprecated) in boost library
#define timer timer_deprecated

#include <boost/progress.hpp>

#undef timer

#include <boost/timer/timer.hpp>
#include <dals.h>
#include <sta.h>
#include <dinic.h>

/////////////////////////////////////////////////////////////////////////////
/// Class ALC, Approximate Local Change
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------
// Getters & Setters
//---------------------------------------------------------------------------
double ALC::GetError() const { return error_; }

ObjPtr ALC::GetTarget() const { return target_; }

ObjPtr ALC::GetSubstitute() const { return substitute_; }

bool ALC::IsComplemented() const { return is_complemented_; }

void ALC::SetError(double err) { error_ = err; }

void ALC::SetTarget(ObjPtr t) { target_ = t; }

void ALC::SetSubstitute(ObjPtr sub) { substitute_ = sub; }

void ALC::SetComplemented(bool is_complemented) { is_complemented_ = is_complemented; }

//---------------------------------------------------------------------------
// ALC Methods
//---------------------------------------------------------------------------
void ALC::Do() {
    if (is_complemented_) {
        inv_ = ObjCreateInv(substitute_);
        ObjReplace(target_, inv_);
    } else
        ObjReplace(target_, substitute_);
}

void ALC::Recover() {
//    Bug: 2 n9 n10 --Do()--> 2 2 n10 --Recover()--> n9 2 n10
//    for (auto const &fan_out : target_fan_outs_) {
//        if (is_complemented_)
//            abc::Abc_ObjPatchFanin(fan_out, inv_, target_);
//        else
//            abc::Abc_ObjPatchFanin(fan_out, substitute_, target_);
//    }
//    if (is_complemented_)
//        ObjDelete(inv_);

    if (is_complemented_)
        ObjDelete(inv_);
    for (auto const &fan_out : target_fan_outs_) {
        abc::Abc_ObjRemoveFanins(fan_out);
        for (auto const &fan_in : target_fan_out_fan_ins_.at(fan_out))
            abc::Abc_ObjAddFanin(fan_out, fan_in);
    }
}

//---------------------------------------------------------------------------
// Operator Methods, Constructors & Destructors
//---------------------------------------------------------------------------
//ALC &ALC::operator=(const ALC &other) {
//    target_ = other.target_;
//    substitute_ = other.substitute_;
//    is_complemented_ = other.is_complemented_;
//    return *this;
//}

ALC::ALC() = default;

ALC::ALC(ObjPtr t, ObjPtr s, bool is_complemented, double error) : error_(error), is_complemented_(is_complemented),
                                                                   target_(t), substitute_(s), inv_(nullptr),
                                                                   target_fan_outs_(std::vector<ObjPtr>()) {
    for (auto const &fan_out : ObjFanouts(target_)) {
        target_fan_outs_.push_back(fan_out);
        for (auto const &fan_in : ObjFanins(fan_out))
            target_fan_out_fan_ins_[fan_out].push_back(fan_in);
    }
}

ALC::~ALC() = default;

/////////////////////////////////////////////////////////////////////////////
/// Singleton Class DALS, Delay-Driven Approximate Logic Synthesis
/////////////////////////////////////////////////////////////////////////////

//---------------------------------------------------------------------------
// Getters & Setters
//---------------------------------------------------------------------------
std::shared_ptr<DALS> DALS::GetDALS() {
    static std::shared_ptr<DALS> dals(new DALS);
    return dals;
}

NtkPtr DALS::GetApproxNtk() { return approx_ntk_; }

void DALS::SetTargetNtk(NtkPtr ntk) {
    target_ntk_ = NtkDuplicate(ntk);
    approx_ntk_ = NtkDuplicate(target_ntk_);
}

void DALS::SetSim64Cycles(int sim_64_cycles) { sim_64_cycles_ = sim_64_cycles; }

//---------------------------------------------------------------------------
// DALS Methods
//---------------------------------------------------------------------------
void DALS::CalcTruthVec(bool show_progress_bar) { truth_vec_ = SimTruthVec(approx_ntk_, show_progress_bar, sim_64_cycles_); }

void DALS::CalcALCs(const std::vector<ObjPtr> &target_nodes, bool show_progress, int top_k) {
    cand_alcs_.clear();

    boost::timer::cpu_timer timer;
    timer.start();
    CalcTruthVec(show_progress);
    if (show_progress)
        std::cout << "Calc TruthVec Finished" << timer.format() << std::endl;

    for (auto const &t_node : target_nodes)
        cand_alcs_.emplace(t_node, std::vector<ALC>());
    auto time_info = CalcSlack(approx_ntk_);
    auto s_nodes = NtkTopoSortPINode(approx_ntk_);

    timer.start();
    // calculate candidate ALCs for each target node
    boost::progress_display *pd = nullptr;
    if (show_progress) pd = new boost::progress_display(cand_alcs_.size());
    for (auto const &t_node : target_nodes) {
        if (show_progress) ++(*pd);
        for (auto const &s_node : s_nodes)
            if (t_node != s_node && time_info.at(s_node).arrival_time < time_info.at(t_node).arrival_time) {
                double est_error = EstSubPairError(t_node, s_node);
                if (time_info.at(s_node).arrival_time < time_info.at(t_node).arrival_time - 1)
                    cand_alcs_[t_node].emplace_back(t_node, s_node, est_error > 0.5, std::min(est_error, 1 - est_error));
                else
                    cand_alcs_[t_node].emplace_back(t_node, s_node, false, est_error);
            }
        if (cand_alcs_[t_node].size() > top_k)
            std::partial_sort(cand_alcs_[t_node].begin(), cand_alcs_[t_node].begin() + top_k, cand_alcs_[t_node].end(),
                              [](const auto &a, const auto &b) {
                                  return a.GetError() < b.GetError();
                              });
        else
            std::sort(cand_alcs_[t_node].begin(), cand_alcs_[t_node].end(),
                      [](const auto &a, const auto &b) {
                          return a.GetError() < b.GetError();
                      });
    }
    if (show_progress)
        std::cout << "Calc Candidate ALCs Finished" << timer.format() << std::endl;

    timer.start();
    // calculate the most optimal ALC for each target node
    if (show_progress) pd = new boost::progress_display(cand_alcs_.size());;
    std::vector<ALC> k_alcs;
    for (auto const &t_node : target_nodes) {
        if (show_progress) ++(*pd);
        k_alcs.clear();
        for (auto alc: cand_alcs_[t_node]) {
            if (k_alcs.size() == top_k) break;
            alc.Do();
            alc.SetError(SimER(target_ntk_, approx_ntk_));
            alc.Recover();
            k_alcs.push_back(alc);
        }
        std::sort(k_alcs.begin(), k_alcs.end(),
                  [](const auto &a, const auto &b) {
                      return a.GetError() < b.GetError();
                  });
        opt_alc_.emplace(t_node, k_alcs.front());
    }
    if (show_progress)
        std::cout << "Calc Optimal ALC Finished" << timer.format() << std::endl;
}

double DALS::EstSubPairError(ObjPtr target, ObjPtr substitute) {
    int err_cnt = 0;
    for (int i = 0; i < sim_64_cycles_; i++)
        err_cnt += std::bitset<64>(truth_vec_.at(target)[i] ^ truth_vec_.at(substitute)[i]).count();
    return (double) err_cnt / (double) (64 * sim_64_cycles_);
}

void DALS::Run(double err_constraint) {
    double err = 0;
    int round = 0;
    while (err < err_constraint) {
        round++;
        auto time_info = CalcSlack(approx_ntk_);

        std::vector<ObjPtr> pis_nodes_0, nodes_0;
        for (auto const &obj : NtkTopoSortPINode(approx_ntk_))
            if (time_info.at(obj).slack == 0) {
                pis_nodes_0.push_back(obj);
                if (ObjIsNode(obj)) nodes_0.push_back(obj);
            }

        CalcALCs(nodes_0, false, 3);

        int N = abc::Abc_NtkObjNumMax(approx_ntk_) + 1;
        int source = 0, sink = N - 1;
        Dinic dinic(N * 2);

        for (const auto &obj_0 : pis_nodes_0) {
            int u = ObjID(obj_0);
            if (ObjIsPI(obj_0))
                dinic.AddEdge(source, u, std::numeric_limits<double>::max());
            else {
                if (opt_alc_.at(obj_0).GetError() == 0)
                    dinic.AddEdge(u, u + N, std::numeric_limits<double>::min());
                else
                    dinic.AddEdge(u, u + N, opt_alc_.at(obj_0).GetError());
                if (ObjIsPONode(obj_0))
                    dinic.AddEdge(u + N, sink, std::numeric_limits<double>::max());
            }
        }

        for (auto &[u, vs] : GetCriticalGraph(approx_ntk_)) {
            for (auto &v : vs) {
                if (ObjIsPI(NtkObjbyID(approx_ntk_, u)))
                    dinic.AddEdge(u, v, std::numeric_limits<double>::max());
                else
                    dinic.AddEdge(u + N, v, std::numeric_limits<double>::max());
            }
        }

//        for (auto &[u, vs] : GetCriticalGraph(approx_ntk_)) {
//            std::cout << u << ": ";
//            for (auto &v : vs) std::cout << v << " ";
//            std::cout << std::endl;
//        }

        std::cout << "---------------------------------------------------------------------------" << std::endl;
        std::cout << "> Round " << round << std::endl;
        std::cout << "---------------------------------------------------------------------------" << std::endl;
        std::cout << "MinCut: " << std::endl;
        for (const auto edge : dinic.MinCut(source, sink)) {
            auto obj = NtkObjbyID(approx_ntk_, edge.u);
            std::cout << ObjName(obj) << "--->" << ObjName(opt_alc_.at(obj).GetSubstitute())
                      << " : " << opt_alc_.at(obj).IsComplemented()
                      << " : " << opt_alc_.at(obj).GetError()
                      << std::endl;
            opt_alc_.at(obj).Do();
        }

        err = SimER(target_ntk_, approx_ntk_);
        std::cout << "Error Rate: " << err << std::endl;
        std::cout << "Delay: "
                  << GetKMostCriticalPaths(target_ntk_, 1)[0].max_delay << "--->"
                  << GetKMostCriticalPaths(approx_ntk_, 1)[0].max_delay << std::endl;

//        std::cout << "Do: " << ObjName(NtkObjbyID(approx_ntk_, 383)) << std::endl;
//        opt_alc_.at(NtkObjbyID(approx_ntk_, 383)).Do();
//        std::cout << "Do: " << ObjName(NtkObjbyID(approx_ntk_, 487)) << std::endl;
//        opt_alc_.at(NtkObjbyID(approx_ntk_, 487)).Do();
//        std::cout << "Error Rate: " << SimER(target_ntk_, approx_ntk_) << std::endl;
//        std::cout << "Delay: "
//                  << GetKMostCriticalPaths(target_ntk_, 1)[0].max_delay << "--->"
//                  << GetKMostCriticalPaths(approx_ntk_, 1)[0].max_delay << std::endl;

//        for (auto &[obj, alc] : opt_alc_) {
//            std::cout << "---------------------------------------------------------------------------" << std::endl;
//            std::cout << "target=" << std::setw(5) << ObjName(alc.GetTarget())
//                      << " sub=" << std::setw(5) << ObjName(alc.GetSubstitute())
//                      << " compl=" << alc.IsComplemented()
//                      << std::fixed
//                      << " est err=" << std::setprecision(4) << std::min(EstSubPairError(alc.GetTarget(), alc.GetSubstitute()),
//                                                                         1 - EstSubPairError(alc.GetTarget(), alc.GetSubstitute()))
//                      << " sim err=" << std::setprecision(4) << alc.GetError()
//                      << " at=" << time_info.at(alc.GetTarget()).arrival_time
//                      << " rt=" << time_info.at(alc.GetTarget()).required_time
//                      << std::endl;
//        }
//        std::cout << "---------------------------------------------------------------------------" << std::endl;
//        break;
    }

//    for (auto &[obj, alc] : opt_alc_) {
//        std::cout << "---------------------------------------------------------------------------" << std::endl;
//        std::cout << "target=" << std::setw(5) << ObjName(alc.GetTarget())
//                  << " sub=" << std::setw(5) << ObjName(alc.GetSubstitute())
//                  << " compl=" << alc.IsComplemented()
//                  << std::fixed
//                  << " est err=" << std::setprecision(4) << std::min(EstSubPairError(alc.GetTarget(), alc.GetSubstitute()),
//                                                                     1 - EstSubPairError(alc.GetTarget(), alc.GetSubstitute()))
//                  << " sim err=" << std::setprecision(4) << alc.GetError()
//                  << " at=" << time_info.at(alc.GetTarget()).arrival_time
//                  << " rt=" << time_info.at(alc.GetTarget()).required_time
//                  << std::endl;
//    }
//    std::cout << "---------------------------------------------------------------------------" << std::endl;

//    for (auto &[obj, alcs] : cand_alcs_) {
//        std::cout << "---------------------------------------------------------------------------" << std::endl;
//        for (auto &alc: alcs) {
//            std::cout << "target=" << std::setw(5) << ObjName(alc.GetTarget())
//                      << " sub=" << std::setw(5) << ObjName(alc.GetSubstitute())
//                      << " compl=" << alc.IsComplemented()
//                      << std::fixed
//                      << " est err=" << std::setprecision(4) << alc.GetError();
//            alc.Do();
//            std::cout << " sim err=" << std::setprecision(4) << SimER(target_ntk_, approx_ntk_)
//                      << " at=" << time_info.at(alc.GetTarget()).arrival_time
//                      << " rt=" << time_info.at(alc.GetTarget()).required_time
//                      << std::endl;
//            alc.Recover();
//        }
//    }
}

//---------------------------------------------------------------------------
// Operator Methods, Constructors & Destructors
//---------------------------------------------------------------------------
DALS::~DALS() {
    NtkDelete(target_ntk_);
    NtkDelete(approx_ntk_);
}

DALS::DALS() = default;
