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

#ifdef __CLING__

#pragma link off all globals;
#pragma link off all classes;
#pragma link off all functions;

#pragma link C++ class o2::fdd::Digit + ;
#pragma link C++ class o2::fdd::ChannelData + ;
#pragma link C++ class vector < o2::fdd::Digit> + ;
#pragma link C++ class vector < o2::fdd::ChannelData> + ;
#pragma link C++ class o2::fdd::Triggers + ;
#pragma link C++ class vector < o2::fdd::Triggers> + ;
#pragma link C++ class o2::fdd::DetTrigInput + ;
#pragma link C++ class vector < o2::fdd::DetTrigInput> + ;

#pragma link C++ class o2::fdd::MCLabel + ;
#pragma link C++ class vector < o2::fdd::MCLabel> + ;

#pragma link C++ class o2::fdd::Hit + ;
#pragma link C++ class vector < o2::fdd::Hit> + ;

#pragma link C++ class o2::fdd::RecPoint + ;
#pragma link C++ class vector < o2::fdd::RecPoint> + ;
#pragma link C++ class o2::fdd::ChannelDataFloat + ;
#pragma link C++ class vector < o2::fdd::ChannelDataFloat> + ;

#pragma link C++ class o2::fdd::RawEventData + ;
#pragma link C++ class o2::fdd::EventHeader + ;
#pragma link C++ class o2::fdd::EventData + ;
#pragma link C++ class o2::fdd::TCMdata + ;
#pragma link C++ class o2::fdd::Topo + ;

#pragma link C++ class o2::fdd::CTFHeader + ;
#pragma link C++ class o2::fdd::CTF + ;
#pragma link C++ class o2::ctf::EncodedBlocks < o2::fdd::CTFHeader, 8, uint32_t> + ;

#endif
