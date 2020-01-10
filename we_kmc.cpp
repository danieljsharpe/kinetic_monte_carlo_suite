/*
File containing functions relating to weighted ensemble sampling kMC (WE-kMC)
*/

#include "kmc_methods.h"
#include <omp.h>
#include <iostream>

using namespace std;

WE_KMC::WE_KMC(const Network &ktn, double tau, int nbins, bool adaptivebins) {

    cout << "wekmc> running WE-kMC with parameters:\n  lag time: " << tau << " \tno. of bins: " \
         << nbins << "\n  adaptive binning (y/n): " << adaptivebins << endl;
    if (!adaptivebins) walkers.reserve(nbins); // quack use proper argument
}

WE_KMC::~WE_KMC() {}

void WE_KMC::run_enhanced_kmc(const Network &ktn) {

    cout << "wekmc> running WE-kMC simulation" << endl;
}
