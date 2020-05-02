/*
File containing kinetic Monte Carlo simulation algorithms to propagate individual trajectories

This file is a part of DISCOTRESS, a software package to simulate the dynamics on arbitrary continuous time Markov chains (CTMCs).
Copyright (C) 2020 Daniel J. Sharpe

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "kmc_methods.h"
#include <random>
#include <queue>
#include <string>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <omp.h>

using namespace std;

Walker::~Walker() {}

/* write trajectory and path quantities to file */
void Walker::dump_walker_info(bool transnpath, bool newpath, bool writetraj) {
    if (curr_node==nullptr) throw exception();
    if (writetraj) {
    ofstream walker_f;
    string walker_fname="walker."+to_string(this->walker_id)+"."+to_string(this->path_no)+".dat";
    if (!newpath) { walker_f.open(walker_fname,ios_base::app);
    } else { walker_f.open(walker_fname,ios_base::trunc); }
    walker_f.setf(ios::right,ios::adjustfield); walker_f.setf(ios::fixed,ios::floatfield); // walker_f.fill('x');
    walker_f << setw(7) << curr_node->node_id << setw(7) << curr_node->comm_id << setw(30) << k;
    walker_f.precision(10); // walker_f.width(18);
    walker_f << setw(60) << t << setw(35) << p << setw(20) << s << endl;
    }
    if (!transnpath) return;
    ofstream tpdistrib_f;
    tpdistrib_f.open("tp_distribns.dat",ios_base::app);
    tpdistrib_f.setf(ios::right,ios::adjustfield); tpdistrib_f.setf(ios::fixed,ios::floatfield);
    tpdistrib_f.precision(10);
    tpdistrib_f << setw(14) << path_no << setw(30) << k << setw(60) << t << setw(35) << p << setw(20) << s << endl;
}

/* reset path quantities */
void Walker::reset_walker_info() {
    k=0; p=-numeric_limits<long double>::infinity(); t=0.; s=0.;
    curr_node=nullptr;
}

Wrapper_Method::Wrapper_Method() {}

Wrapper_Method::~Wrapper_Method() {}

/* sample an initial node (from the B set) and set this node as the starting node of the walker */
Node *Wrapper_Method::get_initial_node(const Network &ktn, Walker &walker, int seed) {

    Node *node_b=nullptr; // sampled starting node
    double pi_B = -numeric_limits<double>::infinity(); // (log) occupation probability of all nodes in initial set B
    vector<pair<Node*,double>> b_probs(ktn.nodesB.size()); // accumulated probs of selecting starting node
    set<Node*>::iterator it_set = ktn.nodesB.begin();
    if (ktn.nodesB.size()==1) { // there is only one node in the starting set
        node_b=*it_set;
        pi_B=(*it_set)->pi;
    } else if (!ktn.initcond) { // no initial condition was set, choose node in set B in proportion to stationary probs
        while (it_set!=ktn.nodesB.end()) {
            pi_B = log(exp(pi_B)+exp((*it_set)->pi));
            it_set++; }
        it_set = ktn.nodesB.begin();
        double cum_prob=0.;
        while (it_set!=ktn.nodesB.end()) {
            cum_prob += exp((*it_set)->pi-pi_B);
            b_probs.push_back(make_pair((*it_set),cum_prob));
            it_set++; }
    } else { // choose node in set B in proportion to specified initial condition probs
        pi_B=0.; // for specified initial condition, sum of probabilities should be unity
        double cum_prob=0.; int i=0;
        while (it_set!=ktn.nodesB.end()) {
            cum_prob += ktn.init_probs[i];
            b_probs.push_back(make_pair(*it_set,cum_prob));
            i++; it_set++;
        }
    }
    if (node_b==nullptr) { // if there was more than one node in B, sample the initial node
        double rand_no = Wrapper_Method::rand_unif_met(seed);
        vector<pair<Node*,double>>::iterator it_vec = b_probs.begin();
        while (it_vec!=b_probs.end()) {
            if ((*it_vec).second>=rand_no) { node_b=(*it_vec).first; break; }
            it_vec++; }
        if (it_vec==b_probs.end()) node_b=(*it_vec).first;
    }
    if (node_b==nullptr) throw exception();
    walker.curr_node=&(*node_b);
    walker.p=node_b->pi-pi_B; // factor in path probability corresponding to initial occupation of node
    if (ktn.nbins>0) walker.visited[node_b->bin_id]=true;
    return node_b;
}

/* function to set the Traj_Method member function to propagate individual trajectories */
void Wrapper_Method::set_standard_kmc(void(*kmcfuncptr)(Walker&)) {
    kmc_func = kmcfuncptr;
}

/* use breadth-first search (BFS) procedure to find a community on-the-fly, based on a maximum size of the community
   and a specified transition rate cutoff */
vector<int> Wrapper_Method::find_comm_onthefly(const Network &ktn, const Node *init_node, \
        double adaptminrate, int maxsz) {

    vector<int> nodes_in_comm(ktn.n_nodes); // store flags indicating if node is of community or is part of absorbing boundary
    queue<int> nbr_queue; // queue of node IDs to visit in the BFS procedure
    nbr_queue.push(init_node->node_id);
    int nv=0; // number of nodes in the community being built up
    while (!nbr_queue.empty() && nv<maxsz) {
        int curr_node_id = nbr_queue.front();
        nbr_queue.pop();
        nodes_in_comm[curr_node_id-1]=2; nv++; // indicates that node is part of the current community
        const Node *nodeptr = &ktn.nodes[curr_node_id-1];
        const Edge *edgeptr = nodeptr->top_from;
        while (edgeptr!=nullptr) {
            if (edgeptr->deadts || nodes_in_comm[edgeptr->to_node->node_id-1]==2) { // removed edge or node already in comm
                edgeptr=edgeptr->next_from; continue; }
            if (exp(edgeptr->k)>adaptminrate && edgeptr->to_node->aorb!=-1) { // queue neighbouring node to be added into community
                if (nodes_in_comm[edgeptr->to_node->node_id-1]==0) { // node is not already queued
                    nbr_queue.push(edgeptr->to_node->node_id);
                }
            }
            // mark node as belonging to absorbing boundary (for now)
            nodes_in_comm[edgeptr->to_node->node_id-1]=3;
            edgeptr=edgeptr->next_from;
        }
    }
    return nodes_in_comm;
}

/* Increment number of A<-B and B<-B paths simulated. If desired, update the vectors containing counts needed to
   calculate transition path statistics for bins */
void Wrapper_Method::update_tp_stats(Walker &walker, bool abpath, bool update) {
    n_traj++; if (abpath) n_ab++;
    if (!update) return;
    int i=0; // bin ID
    for (bool bin_visit: walker.visited) {
        if (bin_visit) {
            if (abpath) { ab_successes[i]++;
            } else { ab_failures[i]++; }
        }
        i++;
    }
    fill(walker.visited.begin(),walker.visited.end(),false);
}

/* calculate the transition path statistics for bins from the observed counts during the simulation */
void Wrapper_Method::calc_tp_stats(int nbins) {
    cout << "wrapper_method> calculating transition path statistics" << endl;
    for (int i=0;i<nbins;i++) {
        committors[i] = static_cast<double>(ab_successes[i])/static_cast<double>(ab_successes[i]+ab_failures[i]);
        tp_densities[i] = static_cast<double>(ab_successes[i])/static_cast<double>(n_ab);
    }
    write_tp_stats(nbins);
}

/* write transition path statistics to file */
void Wrapper_Method::write_tp_stats(int nbins) {
    cout << "wrapper_method> writing transition path statistics to file" << endl;
    ofstream tpstats_f;
    tpstats_f.open("tp_stats.dat");
    for (int i=0;i<nbins;i++) {
        tpstats_f << setw(7) << i << setw(10) << ab_successes[i] << setw(10) << ab_failures[i];
        tpstats_f << fixed << setprecision(12);
        tpstats_f << setw(26) << tp_densities[i] << setw(20) << committors[i] << endl;
    }
}

/* draw a uniform random number between 0 and 1, used in Metropolis conditions etc. */
long double Wrapper_Method::rand_unif_met(int seed) {
    static default_random_engine generator(seed);
    static uniform_real_distribution<long double> unif_real_distrib(0.,1.);
    return unif_real_distrib(generator);
}

/* Wrapper_Method no enhanced sampling method (ie simulate A<-B transition paths using chosen trajectory propagation method */
STD_KMC::STD_KMC(const Network& ktn, int maxn_abpaths, int maxit, double tintvl, bool adaptivecomms, int seed) {
    cout << "std_kmc> setting up kMC simulation with no enhanced sampling wrapper method" << endl;
    // quack move this somewhere more general
    this->walker.accumprobs=ktn.accumprobs;
    this->adaptivecomms=adaptivecomms;
    this->maxn_abpaths=maxn_abpaths; this->maxit=maxit; this->tintvl=tintvl; this->seed=seed;
    if (ktn.ncomms>0) {
        walker.visited.resize(ktn.nbins); fill(walker.visited.begin(),walker.visited.end(),false);
        tp_densities.resize(ktn.nbins); committors.resize(ktn.nbins);
        ab_successes.resize(ktn.nbins); ab_failures.resize(ktn.nbins);
    }
}

STD_KMC::~STD_KMC() {}

/* main loop to drive a standard kMC simulation (no enhanced sampling wrapper method) */
void STD_KMC::run_enhanced_kmc(const Network &ktn, Traj_Method &traj_method_obj) {

    cout << "\nstd_kmc> beginning kMC simulation with no enhanced sampling wrapper method" << endl;
    long double dummy_randno = Wrapper_Method::rand_unif_met(seed); // seed the generator
    n_ab=0; n_traj=0; int n_it=1;
    while ((n_ab<maxn_abpaths) && (n_it<=maxit)) { // if using kPS or MCAMC, algo terminates when max no of basin escapes have been simulated
        bool donebklsteps=false;
        traj_method_obj.kmc_iteration(ktn,walker);
        traj_method_obj.dump_traj(walker,walker.curr_node->aorb==-1,false);
        n_it++;
        check_if_endpoint:
            if (walker.curr_node->aorb==-1 || walker.curr_node->aorb==1) { // traj has reached absorbing macrostate A or has returned to B
                update_tp_stats(walker,walker.curr_node->aorb==-1,!adaptivecomms);
                if (walker.curr_node->aorb==-1) { // transition path, reset walker
                    walker.reset_walker_info();
                    walker.path_no++;
                    traj_method_obj.reset_nodeptrs();
                    continue;
                } else if (ktn.nbins>0) {
                    walker.visited[walker.curr_node->bin_id]=true;
                }
            }
            if (donebklsteps) continue;
        traj_method_obj.do_bkl_steps(ktn,walker);
        donebklsteps=true;
        goto check_if_endpoint;
    }
    cout << "\nstd_kmc> simulation terminated after " << n_it-1 << " iterations. Simulated " \
         << n_ab << " transition paths" << endl;
    if (!adaptivecomms) calc_tp_stats(ktn.nbins); // calc committors and transn path densities for communities and write to file
}

/* Wrapper_Method handle simulation of many short nonequilibrium trajectories, used to obtain data required for coarse-graining
   a transition network */
DIMREDN::DIMREDN(const Network &ktn, vector<int> ntrajsvec, double dt, int seed) {

    cout << "dimredn> constructing DIMREDN class" << endl;
    this->ntrajsvec=ntrajsvec; this->dt=dt;
    this->seed=seed;
    walkers.resize(ktn.ncomms);
}

DIMREDN::~DIMREDN() {}

/* main loop to simulate many short nonequilibrium trajectories of fixed length starting from each community in turn */
void DIMREDN::run_enhanced_kmc(const Network &ktn, Traj_Method &traj_method_obj) {

    cout << "dimredn> beginning simulation to obtain trajectory data for dimensionality reduction" << endl;
    for (int i=0;i<ntrajsvec.size();i++) {
        cout << "i: " << ntrajsvec[i] << endl; }
}

Traj_Method::Traj_Method() {}
Traj_Method::~Traj_Method() {}

void Traj_Method::dump_traj(Walker &walker, bool transnpath, bool newpath) {
    walker.dump_walker_info(transnpath,newpath,transnpath || newpath || tintvl==0. || (tintvl>0. && walker.t>=next_tintvl));
    if (tintvl>0. && walker.t>=next_tintvl) { // reached time interval for dumping trajectory data, calc next interval
        while (walker.t>=next_tintvl) next_tintvl+=tintvl;
    }
}

BKL::BKL(const Network &ktn, double tintvl, int seed) {
    this->tintvl=tintvl; this->seed=seed;
}

BKL::~BKL() {}

/* effectively a dummy wrapper function to bkl() function so that BKL class is consistent with other Traj_Method classes */
void BKL::kmc_iteration(const Network &ktn, Walker &walker) {
    if (walker.curr_node==nullptr) {
        Wrapper_Method::get_initial_node(ktn,walker,seed);
        walker.dump_walker_info(false,true,true);
        next_tintvl=tintvl;
    }
    BKL::bkl(walker);
    if (ktn.nbins>0) walker.visited[walker.curr_node->bin_id]=true;
}

/* function to take a single kMC step using the BKL algorithm */
void BKL::bkl(Walker &walker) {
    // propagate trajectory
    double rand_no = Wrapper_Method::rand_unif_met(); // random number used to select transition
    Edge *edgeptr = walker.curr_node->top_from;
    const Node *old_node = walker.curr_node;
    long double prev_cum_p = 0.; // previous accumulated branching probability
    long double p; // branching probability of accepted move
    while (edgeptr!=nullptr) { // loop over FROM edges and check random number against accumulated branching probability
        if (walker.accumprobs) { // branching probability values are cumulative
            if (edgeptr->t > rand_no) { p = edgeptr->t-prev_cum_p; break; }
            prev_cum_p = edgeptr->t;
        } else {
            if (edgeptr->t+prev_cum_p > rand_no) { p = edgeptr->t; break; }
            prev_cum_p += edgeptr->t;
        }
        edgeptr = edgeptr->next_from;
    }
    walker.curr_node = edgeptr->to_node; // advance trajectory
    // update path quantities
    walker.k++; // dynamical activity (no. of steps)
    walker.p += log(p); // log path probability
    walker.t += -(1./exp(old_node->k_esc))*log(Wrapper_Method::rand_unif_met()); // sample transition time
    walker.s += edgeptr->rev_edge->k-edgeptr->k; // entropy flow
}