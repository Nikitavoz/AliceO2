// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \class EMCALChannelData
/// \brief  Perform the EMCAL bad channel calibration
/// \author Hannah Bossi, Yale University
/// \ingroup EMCALCalib
/// \since Feb 11, 2021

#ifndef CHANNEL_DATA_H_
#define CHANNEL_DATA_H_

#include "DetectorsCalibration/TimeSlotCalibration.h"
#include "DetectorsCalibration/TimeSlot.h"
#include "DataFormatsEMCAL/Cell.h"
#include "EMCALBase/Geometry.h"
#include "CCDB/CcdbObjectInfo.h"

#include "Framework/Logger.h"
#include "CommonUtils/MemFileHelper.h"
#include "CCDB/CcdbApi.h"
#include "DetectorsCalibration/Utils.h"
#include <boost/histogram.hpp>
#include <boost/histogram/ostream.hpp>
#include <boost/format.hpp>

#include <array>
#include <boost/histogram.hpp>

namespace o2
{
namespace emcal
{
struct ChannelCalibInitParams {
  unsigned int nbins = 1000;
  std::array<float, 2> range = {0, 0.35};
};

class EMCALChannelData
{
  //using Slot = o2::calibration::TimeSlot<o2::emcal::EMCALChannelData>;
  using Cells = o2::emcal::Cell;
  using boostHisto = boost::histogram::histogram<std::tuple<boost::histogram::axis::regular<double, boost::use_default, boost::use_default, boost::use_default>, boost::histogram::axis::integer<>>, boost::histogram::unlimited_storage<std::allocator<char>>>;

 public:
  // NCELLS includes DCal, treat as one calibration
  o2::emcal::Geometry* mGeometry = o2::emcal::Geometry::GetInstanceFromRunNumber(300000);
  int NCELLS = mGeometry->GetNCells();

  EMCALChannelData(const ChannelCalibInitParams& hist) : mNBins(hist.nbins), mRange(1)
  {
    // boost histogram with amplitude vs. cell ID, specify the range and binning of the amplitude axis
    mHisto = boost::histogram::make_histogram(boost::histogram::axis::regular<>(mNBins, 0, mRange, "t-texp"), boost::histogram::axis::integer<>(0, NCELLS, "CELL ID"));
  }

  ~EMCALChannelData() = default;

  /// \brief Print relevant info for EMCALChannelData on a given stream
  /// \param stream Stream on which the info is printed on
  /// The function is called in the operator<< providing direct access
  /// to protected members. Explicit calls by the users is normally not
  /// necessary.
  void PrintStream(std::ostream& stream) const;
  /// \brief Print a useful message about the container.
  void print();
  /// \brief Fill the container with the cell ID and amplitude.
  void fill(const gsl::span<const o2::emcal::Cell> data);
  /// \brief Merge the data of two slots.
  void merge(const EMCALChannelData* prev);
  int findBin(float v) const;
  bool hasEnoughData() const;
  boostHisto& getHisto() { return mHisto; }
  const boostHisto& getHisto() const { return mHisto; }

  float getRange() const { return mRange; }
  void setRange(float r) { mRange = r; }

  int getNbins() const { return mNBins; }
  void setNbins(int nb) { mNBins = nb; }

  int getNEvents() const { return mEvents; }
  void setNEvents(int ne) { mEvents = ne; }

 private:
  float mRange = 0.35; // looked at old QA plots where max was 0.35 GeV, might need to be changed
  int mNBins = 1000;
  boostHisto mHisto;
  int mEvents = 1;

  ClassDefNV(EMCALChannelData, 1);
};
/// \brief Printing EMCALChannelData on the stream
/// \param in Stream where the EMCALChannelData is printed on
/// \param emcdata EMCALChannelData to be printed
std::ostream& operator<<(std::ostream& in, const EMCALChannelData& emcdata);

} // end namespace emcal
} // end namespace o2

#endif /*CHANNEL_DATA_H_ */
