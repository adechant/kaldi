// ctc/cctc-training.h

// Copyright       2015  Johns Hopkins University (Author: Daniel Povey)


// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#ifndef KALDI_CTC_CCTC_TRAINING_H_
#define KALDI_CTC_CCTC_TRAINING_H_

#include <vector>
#include <map>

#include "base/kaldi-common.h"
#include "util/common-utils.h"
#include "fstext/fstext-lib.h"
#include "tree/context-dep.h"
#include "lat/kaldi-lattice.h"
#include "matrix/kaldi-matrix.h"
#include "ctc/language-model.h"
#include "ctc/cctc-transition-model.h"
#include "ctc/cctc-supervision.h"
#include "ctc/cctc-tombstone.h"
#include "cudamatrix/cu-matrix.h"
#include "cudamatrix/cu-array.h"

namespace kaldi {
namespace ctc {

// CTC means Connectionist Temporal Classification, see the paper by Graves et
// al.  CCTC means context-dependent CTC, it's an extension of the original model,
// in which the next phone is dependent on the phone history (actually, a truncation
// thereof) in addition to the acoustic history.

struct CctcTrainingOptions {
  BaseFloat denominator_scale;

  CctcTrainingOptions(): denominator_scale(1.0) { }

  void Register(OptionsItf *opts) {
    opts->Register("denominator-scale", &denominator_scale,
                   "Scale on the denominator term in the objective function; "
                   "you can set it to e.g. 0.9 to encourage the probabilities "
                   "to sum to one more closely.");
  }
};


// This class is used while training CCTC models and evaluating probabilities on
// held-out training data.  It is not responsible for the entire process of CCTC
// model training; it is only responsible for the forward-backward from the
// neural net output, and the derivative computation that comes from this
// forward-backward.
// note: the supervision.weight is ignored by this class, you have to apply
// it externally.
class CctcPositiveComputation {
 public:
  CctcPositiveComputation(const CctcTrainingOptions &opts,
                          const CctcTransitionModel &trans_model,
                          const CctcSupervision &supervision,
                          const CuMatrixBase<BaseFloat> &exp_nnet_output,
                          const CuMatrixBase<BaseFloat> &denominators);

  // Does the forward computation.  Returns the total log-prob,
  BaseFloat Forward();

  // Does the backward computation and (efficiently) adds the direct part of the
  // derivative w.r.t. the neural network output to 'nnet_output_deriv' (by
  // 'direct' we mean the term not involving the denominators), and adds the
  // derivative w.r.t. the the denominators to 'log_denominators_deriv'.
  void Backward(CuMatrixBase<BaseFloat> *nnet_output_deriv,
                CuMatrixBase<BaseFloat> *denominators_deriv);


 private:

  const CctcTrainingOptions &opts_;
  const CctcTransitionModel &trans_model_;
  // The supervision object
  const CctcSupervision &supervision_;

  // the exp of the neural net output.
  const CuMatrixBase<BaseFloat> &exp_nnet_output_;
  // the denominators, of dimension nnet_output_.NumRows() by
  // trans_model_.NumHistoryStates(), equal to exp_nnet_output_ * weights_'.
  const CuMatrixBase<BaseFloat> &denominators_;


  // 'fst_indexes' contains an entry for each arc in the supervision FST, in
  // the order you'd get them if you visit each arc of each state in order.
  // The first member of each pair is the index into the
  // numerator_indexes/numerator_probs saying where we need to look up the
  // numerator likelihood; the second is the index into
  // denominator_indexes/denominator_probs, saying where we need to look up
  // the denominator likelihood.
  std::vector<std::pair<int32,int32> > fst_indexes_;
  // This has the same number of elements as fst_indexes_; it's used to store
  // the probabilities on each arc of the FST (these are of the form
  // log(numerator-prob * lm_prob / denominator-prob)), where lm_prob is the
  // phone-language-model probability, taken from the transition model.
  std::vector<BaseFloat> arc_logprobs_;

  // numerator_indexes is a list of indexes that we need to look up in
  // exp_nnet_output_ for the forward-backward computation.  The order is not
  // important, but indexes into this vector appear in .first members in
  // fst_indexes.
  CuArray<Int32Pair> numerator_indexes_;
  // the numerator of the probability.  in the forward computation,
  // numerator_probs_[i] equals exp_nnet_output_(row,column), where (row,column)
  // is the i'th member of numerator_indexes.  In the backward computation,
  // the storage is reused for derivatives.
  Vector<BaseFloat> numerator_probs_;


  // denominator_indexes is a list of indexes that we need to look up in
  // denominators_ for the forward-backward computation.  The order is not
  // important, but indexes into this vector appear in .second members in
  // fst_indexes.
  CuArray<Int32Pair> denominator_indexes_;
  // the denominator of the probability.  denominator_probs_[i] equals
  // exp_nnet_output_(row,column), where (row,column) is the i'th member of
  // denominator_indexes.
  Vector<BaseFloat> denominator_probs_;

  // This quantity, with the same dimension as denominator_probs_, is used in
  // the backward computation to store derivatives w.r.t. the denominator
  // values.
  Vector<BaseFloat> denominator_deriv_;

  // The log-alpha value (forward probability) for each state in the lattice
  Vector<double> log_alpha_;

  // The total log-probability of the supervision (you can interpret this as
  // the posterior of this phone-sequence).
  double tot_log_prob_;

  // The log-beta value (backward probability) for each state in the lattice
  Vector<double> log_beta_;

 private:

  //  This function, called from Forward(), creates fst_indexes_,
  //  numerator_indexes_ and denominator_indexes_.
  void ComputeLookupIndexes();

  // This function, called from Forward(), computes denominator_probs_ and
  // numerator_probs_ via batch lookup operations in exp_nnet_output_ and
  // denominators_, and then computes arc_probs_.
  void LookUpLikelihoods();

  // This function, called from Forward(), does the actual forward-computation on
  // the FST, setting alpha_ and tot_log_prob_.
  void ComputeAlpha();

  // Computes the beta probabilities (called from Backward().)
  void ComputeBeta();

  // Computes derivatives (called from Backward()).
  // Returns true on success, false if a NaN or Inf was detected.
  void ComputeDerivatives(CuMatrixBase<BaseFloat> *nnet_output_deriv,
                          CuMatrixBase<BaseFloat> *denominators_deriv);


};

// This is a wrapping layer for both CctcPositiveComputation and
// CctcNegativeComputation; it does the parts that both share, so
// we can avoid duplication.
class CctcCommonComputation {
 public:
  /// Note: the 'cu_weights' argument should be the output of
  /// trans_model.ComputeWeights().
  ///
  /// The 'num_sequences' should be the number of separate sequences
  /// that the computation contains (i.e. number of separate 'n' values
  /// in the supervision's indexes)... this info has to be provided from
  /// the nnet3 code, as it's not stored at this level.
  CctcCommonComputation(const CctcTrainingOptions &opts,
                        const CctcTransitionModel &trans_model,
                        const CuMatrix<BaseFloat> &cu_weights,
                        const CctcSupervision &supervision,
                        int32 num_sequences,
                        const CuMatrixBase<BaseFloat> &nnet_output);


  // Does the forward part of the computation
  // the objf parts should be added together to get the real objf (including
  // the weighting factor in supervision.weight), and then
  // divided by the denominator (== num-frames * weight) for reporting purposes.
  // Note: negative_objf_part is the likelihood from the CctcNegativeComputation object
  // times -opts_.denominator_scale times supervision.weight.
  void Forward(BaseFloat *positive_objf_part, BaseFloat *negative_objf_part,
               BaseFloat *objf_denominator);

  // Does the backward part of the computation; outputs the derivative to 'nnet_output_deriv'.
  void Backward(CuMatrixBase<BaseFloat> *nnet_output_deriv);

  ~CctcCommonComputation();
 private:
  // This function, called from the constructor, checks various dimensions.
  void CheckDims() const;

  CctcHmm hmm_;
  const CctcTrainingOptions &opts_;
  const CctcTransitionModel &trans_model_;
  // cu_weights_ is derived from trans_model_.  Dimension is
  // trans_model_.NumHistoryStates() by trans_model_.NumOutputIndexes().
  const CuMatrix<BaseFloat> &cu_weights_;
  // The supervision object
  const CctcSupervision &supervision_;
  // The number of separate time-sequences that the supervision object covers,
  // which must all be of the same lengty.  This info has to be computed at the
  // nnet3 level of the code.
  int32 num_sequences_;
  // The neural net output
  const CuMatrixBase<BaseFloat> &nnet_output_;
  // the exponent of the neural net output.
  CuMatrix<BaseFloat> exp_nnet_output_;

  // the denominators, of dimension nnet_output_.NumRows() by
  // trans_model_.NumHistoryStates(), equal to exp_nnet_output_ * weights_'.
  // Equal to exp_nnet_output_ * cu_weights_'.
  CuMatrix<BaseFloat> denominators_;

  // used to store the derivative of the objf w.r.t. the log-denominators and w.r.t. the
  // denominators, at different times.
  CuMatrix<BaseFloat> denominators_deriv_;

  CctcPositiveComputation *positive_computation_;

  CctcNegativeComputation *negative_computation_;


};

}  // namespace ctc
}  // namespace kaldi

#endif  // KALDI_CTC_CCTC_TRAINING_H_
