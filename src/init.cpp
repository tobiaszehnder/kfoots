#include <Rcpp.h>
#include "array.cpp"
#include <algorithm> 
#include <R_ext/BLAS.h>
#include <parallel/algorithm>


//TABBING FUNCTIONS: they perform something similar to table(v) in R, where v is
//an integer vector, with 2 differences:
//1: v has to contain only positive values
//2: the resulting table will contain all contiguous values from 0 to max(v), even if
//		 they do not occur in v
inline void shrink(std::vector<int>& v){
	int realsize = v.size();
	for (; realsize > 0 && v[realsize-1] == 0; --realsize){}
	v.resize(realsize);
}


template<typename TIter>
void tabFast_impl(TIter C, TIter E, std::vector<int>& tab, bool wantshrink=true){
	int csize = tab.size();
	for (; C < E; ++C){
		int c = *C;
		if (c < 0) Rcpp::stop("negative counts are not allowed");
		if (c >= csize) { csize += c; tab.resize(csize); }
		++tab[c];
	}
	if (wantshrink) shrink(tab);
}

//main function
// [[Rcpp::export]]
Rcpp::IntegerVector tabFast(Rcpp::IntegerVector counts){
	std::vector<int> tab(100);
	tabFast_impl(counts.begin(), counts.end(), tab);
	return Rcpp::wrap(tab);
}

//equivalent to apply(counts, 2, tabFast)
// [[Rcpp::export]]
Rcpp::List tabCols(Rcpp::IntegerMatrix counts, int nthreads=1){
	int ncol = counts.ncol();
	nthreads = std::min(ncol, nthreads);
	std::vector<std::vector<int>> result(ncol);
	#pragma omp parallel for num_threads(nthreads)
	for (int i = 0; i < ncol; ++i){
		Rcpp::MatrixColumn<INTSXP> col = counts.column(i);
		result[i].resize(100);
		tabFast_impl(col.begin(), col.end(),result[i]);
	}
	return Rcpp::wrap(result);
}
//equivalent to apply(counts, 1, tabFast)
// [[Rcpp::export]]
Rcpp::List tabRows(Rcpp::IntegerMatrix counts, int nthreads=1){
	int nrow = counts.nrow();
	int ncol = counts.ncol();
	nthreads = std::min(nrow, nthreads);
	std::vector<std::vector<int>> result(nrow);
	for (int i = 0; i < nrow; ++i) result[i].resize(100);
	int chunkSize = 1e5/nrow;
	int chunkStart = 0;
	while (chunkStart < ncol){
		int chunkEnd = std::min(chunkStart + chunkSize, ncol);
		#pragma omp parallel for num_threads(nthreads)
		for (int i = 0; i < nrow; ++i){
			Rcpp::MatrixRow<INTSXP> row = counts.row(i);
			tabFast_impl(row.begin() + chunkStart, row.begin() + chunkEnd, result[i], false);
		}
		chunkStart = chunkEnd;
	}
	
	for (int i = 0; i < nrow; ++i) shrink(result[i]);
	
	return Rcpp::wrap(result);
}

//END TABBING FUNCTIONS
//method for doing quantile normalization between an empirical and a theoretical distribution
//cfreq is the cumulative frequency of some empirical counts (position zero corresponds to count zero and so on)
//cdf is a callable function or class such that cdf(i) gives the cumulative prob
//ass is an assignment of the same length of cfreq (specified by flen) that assigns an observed count
//to a theoretical count
//there are many ways of dealing with discretization, here 
//we compute the assignment such that the norm ||CDF_empirical_after_assignment - CDF_theoretical||_1 is minimized.
template<typename Tfun>
void quantNormDiscrete_inner(double* cfreq, int* ass, int flen, Tfun cdf){
	int j = 0;
	bool lastSep = false;
	if (flen <= 0) return;
	if (abs(cfreq[flen-1]-1) >= 1e-10) Rcpp::stop("cumulative frequencies not tending to one");
	for (int i = 0; !lastSep; ++i){
		double P = cdf(i);
		if (P < 0 && P > 1) Rcpp::stop("Invalid cumulative distribution function");
		while (cfreq[j] < P && j < flen) ++j;
		//std::cout << "j: " << j << " i: " << i << std::endl;
		double diffMinus = j==0?P:(P-cfreq[j-1]);
		double diffPlus = cfreq[j] - P;
		//std::cout << "P: " << P << " cfreq[j]: " << cfreq[j] << std::endl;
		//std::cout << "diffMinus: " << diffMinus << " diffPlus: " << diffPlus << std::endl;
		if (diffMinus < diffPlus){
			++ass[j];
			//std::cout << "diffMinus" << std::endl;
		} else {
			//std::cout << "diffPlus" << std::endl;
			if (j >= flen-1) lastSep = true;
			else ++ass[j+1];
		}
	}
	
	int currPoint = 0;
	for (int i = 0; i < flen; ++i){
		currPoint += ass[i];
		ass[i] = currPoint;
	}
}



template<typename Titer>
std::vector<double> getCFreq(Titer begin, Titer end){
	int flen = end-begin;
	std::vector<double> cfreq(flen);
	long double acc = 0;
	for (int i = 0; begin < end; ++i, ++begin){
		acc += *begin;
		cfreq[i] = acc;
	}
	if (acc != 1){//make sure that frequencies sum up to 1
		for (int i = 0; i < flen; ++i) cfreq[i] /= acc;
	}
	
	return cfreq;
}


struct theoCDF {
	std::vector<double>& cdf;
	double operator() (int i){
		if (i < 0) return 0;
		if (i >= ((int)cdf.size())) return 1;
		else return cdf[i];
	}
	theoCDF(std::vector<double>& acdf) : cdf(acdf){}
};

struct poisCDF {
	double lambda;
	double operator() (int i){
		return Rf_ppois(i, lambda, true, false);
	}
};

//quantile normalization of the empirical distribution (just a vector of count frequencies)
//according to a theoretical distribution (also a vector of count frequenceis)

// [[Rcpp::export]]
Rcpp::IntegerVector labelCounts(Rcpp::NumericVector empirical, Rcpp::NumericVector theoretical){
	std::vector<double> cfreqE = getCFreq(empirical.begin(), empirical.end());
	std::vector<double> cfreqT = getCFreq(theoretical.begin(), theoretical.end());
	
	theoCDF cdf(cfreqT);
	int flen = empirical.length();
	Rcpp::IntegerVector assignment(flen);
	quantNormDiscrete_inner(cfreqE.data(), assignment.begin(), flen, cdf);
	return assignment;
}


//coords and counts have the same format
//clusters is an object to form clusters (seeds) based on the counts of a single histone mark
//typically clusters is produced by doing something like: 
//clusters <- apply(counts, 1, function(row) labelCounts(kfoots:::tabFast(row), theoretical=rep(0.1, 10)))
//each column of the counts matrix will be assigned to nrow(counts) different seeds,
//there are nrow(counts) independent sets of seeds
//column i, when looking at row j, will be assigned to the seed in the set j specified by clusters[[j]][coords[i,j] + 1]
// [[Rcpp::export]]
Rcpp::List clusterAverages2(Rcpp::IntegerMatrix counts, Rcpp::IntegerMatrix coords, Rcpp::List clusters, int nthreads=1){
	if (coords.ncol()!=counts.ncol()||coords.nrow()!=counts.nrow()) Rcpp::stop("counts and coords must have the same format");
	int ncomp = counts.nrow();
	int ncol = counts.ncol();
	if (ncomp != clusters.length()) Rcpp::stop("one set of clusters for each row of the count matrix is required");
	//switch to the array of arrays format
	std::vector<Rcpp::IntegerVector> Cs;
	for (int i = 0; i < ncomp; ++i) {
		Rcpp::IntegerVector v(clusters[i]);
		Cs.push_back(v);
	}
	//determine the maximum number of different clusters per row of count matrix
	int maxclust = 0;
	for (int i = 0; i < ncomp; ++i){
		int m = *(std::max_element(Cs[i].begin(), Cs[i].end()));
		maxclust = std::max(maxclust, m);
	}
	maxclust += 1;
	
	//allocate memory
	Rcpp::NumericMatrix mus(ncomp, maxclust*ncomp);
	Rcpp::IntegerVector sizes(maxclust*ncomp);
	
	//parallelize on the columns of the matrix
	#pragma omp parallel num_threads(nthreads)
	{
		//accumulators local to this thread
		std::vector<long> vsum(ncomp*maxclust*ncomp);
		std::vector<int> vnum(ncomp*maxclust);
		#pragma omp for nowait
		for (int i = 0; i < ncol; ++i){
			Rcpp::MatrixColumn<INTSXP> tcol = counts.column(i);
			Rcpp::MatrixColumn<INTSXP> dcol = coords.column(i);
			for (int j = 0; j < ncomp; ++j){
				int c = dcol[j];
				if (c < 0 || c >= Cs[j].length()) Rcpp::stop("invalid clustering or invalid counts");
				int clust = Cs[j][c];
				if (clust < 0 || clust >= maxclust) Rcpp::stop("something wrong in detecting maxclust");
				clust = (j*maxclust + clust);
				int offset = clust*ncomp;
				for (int k = 0; k < ncomp; ++k){
					vsum[offset + k] += tcol[k];
				}
				++vnum[clust];
			}
		}
		//threads accumulator update the global accumulators in a protected section
		#pragma omp critical
		{
			for (int i = 0, e = vsum.size(); i < e; ++i){
				mus[i] += vsum[i];
			}
			for (int i = 0, e = vnum.size(); i < e; ++i){
				sizes[i] += vnum[i];
			}
		}
	}
	for (int i = 0, e = mus.ncol(); i < e; ++i){
		int n = sizes[i];
		if (n > 0){
			for (int j = 0; j < ncomp; ++j){
				mus(j, i) /= n;
			}
		}
	}
	return Rcpp::List::create(Rcpp::Named("mus")=mus, Rcpp::Named("sizes")=sizes);
}


// [[Rcpp::export]]
Rcpp::List clusterAverages(Rcpp::IntegerMatrix counts, Rcpp::List clusters, int nthreads=1){
	return clusterAverages2(counts, counts, clusters, nthreads);
}

//fill it in, written in c:
//for (i in 1:ncol(counts)){
//	for (j in 1:nrow(counts)){
//		clust <- multidclust[[j]][counts[j,i]+1] + 1
//		posteriors[clust,i] <- posteriors[clust,i] + 1
//	}
//}

// [[Rcpp::export]]
void fillPosteriors(Rcpp::IntegerMatrix coords, Rcpp::List clusters, Rcpp::NumericMatrix posteriors, int nthreads=1){
	int ncomp = coords.nrow();
	int ncol = coords.ncol();
	if (ncomp != clusters.length()) Rcpp::stop("one set of clusters for each row of the count matrix is required");
	if (ncol != posteriors.ncol()) Rcpp::stop("posteriors matrix doesn't match with the count matrix");
	int nclust = posteriors.nrow();
	
	//switch to the std::vector of Rcpp::vector format
	std::vector<Rcpp::IntegerVector> Cs;
	for (int i = 0; i < ncomp; ++i) {
		Rcpp::IntegerVector v(clusters[i]);
		Cs.push_back(v);
	}
	
	//parallelize on the columns of the matrix
	#pragma omp parallel for num_threads(nthreads)
	for (int i = 0; i < ncol; ++i){
		Rcpp::MatrixColumn<INTSXP> ccol = coords.column(i);
		Rcpp::MatrixColumn<REALSXP> pcol = posteriors.column(i);
		for (int j = 0; j < ncomp; ++j){
			int c = ccol[j];
			if (c < 0 || c >= Cs[j].length()) Rcpp::stop("invalid clustering or invalid coords");
			int clust = Cs[j][c];
			if (clust < 0 || clust >= nclust) Rcpp::stop("count mapped to invalid cluster");
			++pcol[clust];
		}
	}
}

template<typename TVec1, typename TVec2, typename TCoeff>
inline void addCol(int n, TVec1 v1, TVec2 v2, TCoeff c){
	for (int i = 0; i < n; ++i) v1[i] += c*v2[i];
}

template<typename TVec1, typename TVec2>
inline void addAllCols(int n, TVec1 v1, TVec2 v2){
	for (int i = n; i > 0; --i) addCol(i, v1 + (i-1)*n, v2, v2[i-1]);
}


// [[Rcpp::export]]
Rcpp::NumericMatrix rowcov(Rcpp::NumericMatrix counts, bool besselCorr=true, int nthreads=1){
	int ncol = counts.ncol();
	int nrow = counts.nrow();
	std::vector<long double> std_cov(nrow*nrow);
	Mat<long double> cov = asMat(std_cov, nrow);
	Mat<double> CNTS = asMat(counts);
	
	#pragma omp parallel num_threads(nthreads)
	{
		std::vector<long double> t_std_cov(nrow*nrow);
		long double* t_ptr_cov = t_std_cov.data();
		Mat<long double> t_cov = asMat(t_std_cov, nrow);
		#pragma omp for nowait
		for (int col = 0; col < ncol; ++col){
			addAllCols(nrow, t_ptr_cov, CNTS.colptr(col));
		}
		#pragma omp critical
		{
			for (int j = 0; j < nrow; ++j){
				for (int i = 0; i <= j; ++i){
					cov(i,j) += t_cov(i,j);
				}
			}
		}
	}
	Rcpp::NumericMatrix result(nrow, nrow);
	long double denom = besselCorr?(ncol-1):ncol;
	for (int j = 0; j < nrow; ++j){
		for (int i = 0; i <= j; ++i){
			result(i,j) = result(j,i) = cov(i,j)/denom;
		}
	}
	return result;
}


//matrix multiplication. Parallelization is done on the columns of mat2 and mat3
//that works only if you don't have to allocate mat3, otherwise R is faster
// [[Rcpp::export]]
void matprod(Rcpp::NumericMatrix mat1, Rcpp::NumericMatrix mat2, Rcpp::NumericMatrix mat3, int nthreads=1){
	int ncol1 = mat1.ncol();
	int nrow1 = mat1.nrow();
	int ncol2 = mat2.ncol();
	int nrow2 = mat2.nrow();
	int ncol3 = mat3.ncol();
	int nrow3 = mat3.nrow();
	if (ncol1 != nrow2 || nrow1 != nrow3 || ncol2 != ncol3) Rcpp::stop("non conformable matrices");
	
	double* M1 = mat1.begin();
	double* M2 = mat2.begin();
	double* M3 = mat3.begin();
	const char *transa = "N", *transb = "N";
	double one = 1.0, zero = 0.0;
	std::vector<int> breaks(nthreads+1);
	for (int i = 0; i <= nthreads; ++i){
		breaks[i] = round(i*(((double)ncol3)/nthreads));
	}
	#pragma omp parallel for num_threads(nthreads)
	for (int i = 0; i < nthreads; ++i){
		double* TM2 = M2 + breaks[i]*nrow2;
		double* TM3 = M3 + breaks[i]*nrow3;
		int ncol23 = breaks[i+1]-breaks[i];
		F77_CALL(dgemm)(transa, transb, &nrow1, &ncol23, &ncol1, &one,
				M1, &nrow1, TM2, &nrow2, &zero, TM3, &nrow1);
	}
}


template<typename TIter>
inline void rangeImpl(double* min, double* max, TIter C, TIter E){
	for (; C < E; ++C){
		double c = *C;
		if (c < *min) *min = c;
		if (c > *max) *max = c;
	}
}

// [[Rcpp::export]]
Rcpp::IntegerMatrix discretizeRows(Rcpp::NumericMatrix scores, int nlevels, int nthreads=1){
	int nrow = scores.nrow();
	int ncol = scores.ncol();
	
	std::vector<double> mins(nrow, std::numeric_limits<double>::infinity());
	std::vector<double> maxs(ncol, -std::numeric_limits<double>::infinity());
	int chunkSize = 1e5/nrow; if (chunkSize < 1) chunkSize = 1;
	int nchunks = ceil(scores.ncol()/((double)chunkSize));
	#pragma omp parallel num_threads(nthreads)
	{	
		std::vector<double> tmins(mins);
		std::vector<double> tmaxs(maxs);
		#pragma omp for nowait
		for (int i = 0; i < nchunks; ++i){
			int chunkStart = chunkSize*i;
			int chunkEnd = std::min(chunkStart + chunkSize, ncol);
			for (int j = 0; j < nrow; ++j){
				Rcpp::MatrixRow<REALSXP>::iterator rowIter = scores.row(j).begin();
				rangeImpl(&mins[j], &maxs[j], rowIter + chunkStart, rowIter + chunkEnd);
			}
		}
		#pragma omp critical
		{
			for (int j = 0; j < nrow; ++j){
				mins[j] = std::min(mins[j], tmins[j]);
				maxs[j] = std::max(maxs[j], tmaxs[j]);
			}
		}
	}
	
	//overwrite the maxs with the deltas
	std::vector<double>& deltas = maxs;
	for (int j = 0; j < nrow; ++j){
		deltas[j] = (maxs[j]-mins[j]) / nlevels;
	}
	
	
	Rcpp::IntegerMatrix mat(nrow, ncol);
	Mat<double> orig = asMat(scores);
	Mat<int> neww = asMat(mat);
	#pragma omp parallel for num_threads(nthreads)
	for (int i = 0; i < ncol; ++i){
		double* O = orig.colptr(i);
		int* N = neww.colptr(i);
		for (int j = 0; j < nrow; ++j){
			N[j] = (O[j] - mins[j])/deltas[j];
		}
	}
	
	mat.attr("dimnames") = scores.attr("dimnames");
	return mat;
}

// [[Rcpp::export]]
Rcpp::IntegerMatrix splitAxes(Rcpp::NumericMatrix scores, int nsplit, int nthreads=1){
	int nrow = scores.nrow();
	int ncol = scores.ncol();
	
	//allocate the matrix to be returned
	Rcpp::IntegerMatrix mat(nrow, ncol);
	
	//allocate the necessary memory
	std::vector<std::pair<double,int> > avatars(ncol);
	for (int j = 0; j < nrow; ++j){
		Rcpp::MatrixRow<REALSXP> scrow = scores.row(j);
		Rcpp::MatrixRow<INTSXP> matrow = mat.row(j);
		//fill in the avatars vector
		#pragma omp parallel for num_threads(nthreads)
		for (int i = 0; i < ncol; ++i){
			avatars[i].first = scrow[i];
			avatars[i].second = i;
		}
		//sort it
		__gnu_parallel::sort(avatars.begin(), avatars.end(), __gnu_parallel::parallel_tag(nthreads));
		//fill in the matrix
		double denom = ncol;
		#pragma omp parallel for num_threads(nthreads)
		for (int i = 0; i < ncol; ++i){
			int a = avatars[i].second;
			matrow[a] = floor(nsplit*(i/denom));
		}
	}
	
	mat.attr("dimnames") = scores.attr("dimnames");
	return mat;
}

// [[Rcpp::export]]
Rcpp::IntegerMatrix splitAxesInt(Rcpp::IntegerMatrix scores, int nsplit, int nthreads=1){
	int nrow = scores.nrow();
	int ncol = scores.ncol();
	
	//allocate the matrix to be returned
	Rcpp::IntegerMatrix mat(nrow, ncol);
	
	//tabulate the values for each row
	Rcpp::List tabs = tabRows(scores, nthreads=nthreads);
	
	#pragma omp parallel for num_threads(nthreads)
	for (int j = 0; j < nrow; ++j){
		Rcpp::MatrixRow<INTSXP> scrow = scores.row(j);
		Rcpp::MatrixRow<INTSXP> matrow = mat.row(j);
		//get abundance of each symbol
		Rcpp::IntegerVector tab = tabs[j];
		//we need the cumulative sums of it
		int acc = 0;
		for (int i = 0, e = tab.length(); i < e; ++i){
			int tmp = tab[i];
			tab[i] = acc;
			acc += tmp;
		}
		//fill in mat
		double denom = ncol;
		for (int i = 0; i < ncol; ++i){
			int pos = tab[scrow[i]]++;
			matrow[i] = floor(nsplit*(pos/denom));
		}
	}
	
	mat.attr("dimnames") = scores.attr("dimnames");
	return mat;
}
