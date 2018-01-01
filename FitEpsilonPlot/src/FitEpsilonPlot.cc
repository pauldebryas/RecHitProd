// -*- C++ -*-
//
// Package:    FitEpsilonPlot
// Class:      FitEpsilonPlot
// 
/**\class FitEpsilonPlot FitEpsilonPlot.cc CalibCode/FitEpsilonPlot/src/FitEpsilonPlot.cc

Description: [one line class summary]

Implementation:
[Notes on implementation]
 */
//
// Original Author:  Marco Grassi, CMS
//         Created:  Tue Nov  8 17:18:54 CET 2011
// $Id: FitEpsilonPlot.cc,v 1.9 2013/06/17 13:40:42 lpernie Exp $
//
//

// system include files
#include <memory>
#include <iostream>
#include <string>

#include "TF1.h"
#include "TH1F.h"
#include "TH2F.h"
#include "TFile.h"
#include "TTree.h"
#include "TLatex.h"
#include "TMath.h"
#include "TCanvas.h"
#include "TPaveStats.h"
#include "TFitResult.h"
#include "TLegend.h"
#include "TROOT.h"
#include "TStyle.h"

// user include files
#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/EDAnalyzer.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Utilities/interface/Exception.h"

#include "DataFormats/EcalDetId/interface/EBDetId.h"
#include "DataFormats/EcalDetId/interface/EEDetId.h"

#include "RooGaussian.h"
#include "RooChebychev.h"
#include "RooPolynomial.h"
#include "RooDataHist.h"
#include "RooAbsPdf.h"
#include "RooAddPdf.h"
#include "RooArgSet.h"
#include "RooArgList.h"
#include "RooPlot.h"
#include "RooFitResult.h"
#include "RooNLLVar.h"
#include "RooChi2Var.h"
#include "RooMinuit.h"
#include "RooMinimizer.h"

#include "CalibCode/FitEpsilonPlot/interface/FitEpsilonPlot.h"

using std::cout;
using std::endl;
using std::string;
using namespace RooFit;


// in this code the upper boundary of mass for the fit is used to assess the goodness of fit: look for --> if( fitres.chi2 < 5 && fabs(mean-<some_number>)>0.0000001)
// which means "if the Chi2 is good and the mean of the fit is far from the upper boundary ..."
// the upper boundary must be consistent with <some_number>
static double upper_bound_pi0mass_EB = 0.15;
static double upper_bound_pi0mass_EE = 0.16;
static double upper_bound_etamass_EB = 0.62;
static double upper_bound_etamass_EE = 0.62;

FitEpsilonPlot::FitEpsilonPlot(const edm::ParameterSet& iConfig)

{

    //now do what ever initialization is needed
    currentIteration_ =  iConfig.getUntrackedParameter<int>("CurrentIteration");
    epsilonPlotFileName_ = iConfig.getUntrackedParameter<std::string>("EpsilonPlotFileName");
    outputDir_ = iConfig.getUntrackedParameter<std::string>("OutputDir");
    outfilename_          = iConfig.getUntrackedParameter<std::string>("OutputFile");
    calibMapPath_ = iConfig.getUntrackedParameter<std::string>("calibMapPath");
    inRangeFit_ = iConfig.getUntrackedParameter<int>("NInFit");
    finRangeFit_ = iConfig.getUntrackedParameter<int>("NFinFit");    
    EEoEB_ = iConfig.getUntrackedParameter<std::string>("EEorEB");
    isNot_2010_ = iConfig.getUntrackedParameter<bool>("isNot_2010");
    Are_pi0_ = iConfig.getUntrackedParameter<bool>("Are_pi0");
    StoreForTest_ = iConfig.getUntrackedParameter<bool>("StoreForTest",true);
    Barrel_orEndcap_ = iConfig.getUntrackedParameter<std::string>("Barrel_orEndcap");
    useMassInsteadOfEpsilon_ = iConfig.getUntrackedParameter<bool>("useMassInsteadOfEpsilon",true);
    isEoverEtrue_ = iConfig.getUntrackedParameter<bool>("isEoverEtrue",false);


    fitFileName_ = outfilename_;
    std::string strToReplace = "calibMap";
    fitFileName_.replace(outfilename_.find(strToReplace.c_str()),strToReplace.size(),"fitRes");
    fitFileName_ = outputDir_ + "/" + fitFileName_;

    inputEpsilonFile_ = nullptr; //  before we open the file, we assign a null value
    outfile_ = nullptr;

    /// setting calibration type
    calibTypeString_ = iConfig.getUntrackedParameter<std::string>("CalibType");
    if(     calibTypeString_.compare("xtal")    == 0 ) { 
      calibTypeNumber_ = xtal;    
      regionalCalibration_ = &xtalCalib; 
      if (isEoverEtrue_) regionalCalibration_g2_ = &xtalCalib; 
    } else if(calibTypeString_.compare("tt")      == 0 ) { 
      calibTypeNumber_ = tt;      
      regionalCalibration_ = &TTCalib;   
      if (isEoverEtrue_) regionalCalibration_g2_ = &TTCalib; 
    } else if(calibTypeString_.compare("etaring") == 0 ) { 
      calibTypeNumber_ = etaring; 
      regionalCalibration_ = &etaCalib;  
      if (isEoverEtrue_) regionalCalibration_g2_ = &etaCalib; 
    } else throw cms::Exception("CalibType") << "Calib type not recognized\n";

    cout << "FIT_EPSILON: crosscheck: selected type: " << regionalCalibration_->printType() << endl;

    /// retrieving calibration coefficients of the previous iteration
    // if currentIteration_ = 0, calibMapPath_ contains "iter_-1" unless the current set of ICs was started from another existing set (see parameters.py)
    // therefore, the case with extension is included below
    std::string stringToMatch = "iter_-1";  // used below: this string should not match to trigger true condition 
    if(currentIteration_ < 0) throw cms::Exception("IterationNumber") << "Invalid negative iteration number\n";
    else if(currentIteration_ > 0 || (currentIteration_ == 0 && calibMapPath_.find(stringToMatch)==std::string::npos))
    {
	  regionalCalibration_->getCalibMap()->loadCalibMapFromFile(calibMapPath_.c_str());
	  if (isEoverEtrue_) regionalCalibration_g2_->getCalibMap()->loadCalibMapFromFile(calibMapPath_.c_str(),true);
    }

    // load epsilon from current iter
    if (isEoverEtrue_) {

      if ((Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" )) {
	EoverEtrue_g1_EB_h = new TH1F*[regionalCalibration_->getCalibMap()->getNRegionsEB()];
	EoverEtrue_g2_EB_h = new TH1F*[regionalCalibration_g2_->getCalibMap()->getNRegionsEB()];
      }
      if ((Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" )) {
	EoverEtrue_g1_EE_h = new TH1F*[regionalCalibration_->getCalibMap()->getNRegionsEE()];
	EoverEtrue_g2_EE_h = new TH1F*[regionalCalibration_g2_->getCalibMap()->getNRegionsEE()];
      }
      cout << "FIT_EPSILON: FitEpsilonPlot:: loading EoverEtrue plots from file: " << epsilonPlotFileName_ << endl;
      loadEoverEtruePlot(epsilonPlotFileName_,1);
      loadEoverEtruePlot(epsilonPlotFileName_,2);

    } else {

      if ((Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" )) {
	epsilon_EB_h = new TH1F*[regionalCalibration_->getCalibMap()->getNRegionsEB()];
      }
      if ((Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" )) {
	epsilon_EE_h = new TH1F*[regionalCalibration_->getCalibMap()->getNRegionsEE()];
      }
      cout << "FIT_EPSILON: FitEpsilonPlot:: loading epsilon plots from file: " << epsilonPlotFileName_ << endl;
      loadEpsilonPlot(epsilonPlotFileName_);

    }

}


FitEpsilonPlot::~FitEpsilonPlot()
{

  if ((Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" )) {

    if (isEoverEtrue_) {
      deleteEpsilonPlot(EoverEtrue_g1_EB_h, regionalCalibration_->getCalibMap()->getNRegionsEB() );
      deleteEpsilonPlot(EoverEtrue_g2_EB_h, regionalCalibration_g2_->getCalibMap()->getNRegionsEB() );
      // delete EoverEtrue_g1_EB_h;
      // delete EoverEtrue_g2_EB_h;
    } else {
      deleteEpsilonPlot(epsilon_EB_h, regionalCalibration_->getCalibMap()->getNRegionsEB() );
      // delete epsilon_EB_h;
    }

  }

  if ((Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" )) {

    if (isEoverEtrue_) {
      deleteEpsilonPlot(EoverEtrue_g1_EE_h, regionalCalibration_->getCalibMap()->getNRegionsEE() );
      deleteEpsilonPlot(EoverEtrue_g2_EE_h, regionalCalibration_g2_->getCalibMap()->getNRegionsEE() );
      // delete EoverEtrue_g1_EE_h;
      // delete EoverEtrue_g2_EE_h;
    } else {
      deleteEpsilonPlot(epsilon_EE_h, regionalCalibration_->getCalibMap()->getNRegionsEE() );
      // delete epsilon_EE_h;
    }

  }

  if (inputEpsilonFile_->IsOpen())
    inputEpsilonFile_->Close();

}


//
// member functions
//


void FitEpsilonPlot::loadEoverEtruePlot(const std::string& filename, const int whichPhoton = 1) {

  // here regionalCalibration_ is only used to get the number of regions, which is the same for both photons
  // hence, no need to use regionalCalibration_ or regionalCalibration_g2_

  std::string line = "";
  std::string histoNamePattern = Form("%s/EoverEtrue_g%d",EEoEB_.c_str(),whichPhoton );

  // open the file if it has not been created so far, otherwise check that it is still open (this would happen on second photon)
  if (inputEpsilonFile_ == nullptr) {
    inputEpsilonFile_ = TFile::Open(filename.c_str());
    if(!inputEpsilonFile_) 
      throw cms::Exception("loadEpsilonPlot") << "Cannot open file " << filename << "\n"; 
  } else if (not inputEpsilonFile_->IsOpen()) {
    inputEpsilonFile_ = TFile::Open(filename.c_str());
  }

  if ( EEoEB_ == "Barrel" && (Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" ) ) {
    
    for (int iR=inRangeFit_; iR <= finRangeFit_ && iR < regionalCalibration_->getCalibMap()->getNRegionsEB(); iR++) {

      line = Form("%s_EB_iR_%d",histoNamePattern.c_str(), iR);

      if (whichPhoton == 1) {
	EoverEtrue_g1_EB_h[iR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());      
	if(!EoverEtrue_g1_EB_h[iR])
	  throw cms::Exception("loadEoverEtruePlot") << "Cannot load histogram " << line << "\n";
	else if(!(iR%1000))
	  cout << "FIT_EPSILON: EoverEtrue distribution (photon " << whichPhoton << ") for EB region " << iR << " loaded" << endl;
      } else {
	EoverEtrue_g2_EB_h[iR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());      
	if(!EoverEtrue_g2_EB_h[iR])
	  throw cms::Exception("loadEoverEtruePlot") << "Cannot load histogram " << line << "\n";
	else if(!(iR%1000))
	  cout << "FIT_EPSILON: EoverEtrue distribution (photon " << whichPhoton << ") for EB region " << iR << " loaded" << endl;
      }
      
    }

  } else if( EEoEB_ == "Endcap" && (Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" ) ) {

    for (int jR=inRangeFit_; jR <= finRangeFit_ && jR<EEDetId::kSizeForDenseIndexing; jR++) {
      
      line = Form("%s_EE_iR_%d",histoNamePattern.c_str(), jR);

      if (whichPhoton == 1) {

	EoverEtrue_g1_EE_h[jR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());
	if(!EoverEtrue_g1_EE_h[jR])
	  throw cms::Exception("loadEoverEtruePlot") << "Cannot load histogram " << line << "\n";
	else if(!(jR%1000))
	  cout << "FIT_EPSILON: EoverEtrue distribution (photon " << whichPhoton << ") for EE region " << jR << " loaded" << endl;

      } else {

	EoverEtrue_g2_EE_h[jR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());
	if(!EoverEtrue_g2_EE_h[jR])
	  throw cms::Exception("loadEoverEtruePlot") << "Cannot load histogram " << line << "\n";
	else if(!(jR%1000))
	  cout << "FIT_EPSILON: EoverEtrue distribution (photon " << whichPhoton << ") for EE region " << jR << " loaded" << endl;
	
      }

    }

  }

}


void FitEpsilonPlot::loadEpsilonPlot(const std::string& filename)
{
  std::string line = "";

  inputEpsilonFile_ = TFile::Open(filename.c_str());
  if(!inputEpsilonFile_) 
    throw cms::Exception("loadEpsilonPlot") << "Cannot open file " << filename << "\n"; 
  if( EEoEB_ == "Barrel" && (Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" ) ){
    for(int iR=inRangeFit_; iR <= finRangeFit_ && iR < regionalCalibration_->getCalibMap()->getNRegionsEB(); iR++)
      {
	line = Form("Barrel/epsilon_EB_iR_%d",iR);
	epsilon_EB_h[iR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());

	if(!epsilon_EB_h[iR])
	  throw cms::Exception("loadEpsilonPlot") << "Cannot load histogram " << line << "\n";
	else if(!(iR%1000))
	  cout << "FIT_EPSILON: Epsilon distribution for EB region " << iR << " loaded" << endl;
      }
  }
  else if( EEoEB_ == "Endcap" && (Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" ) ){
    for(int jR=inRangeFit_; jR <= finRangeFit_ && jR<EEDetId::kSizeForDenseIndexing; jR++)
      {
	line = Form("Endcap/epsilon_EE_iR_%d",jR);
	epsilon_EE_h[jR] = (TH1F*)inputEpsilonFile_->Get(line.c_str());
	if(!epsilon_EE_h[jR])
	  throw cms::Exception("loadEpsilonPlot") << "Cannot load histogram " << line << "\n";
	else if(!(jR%1000))
	  cout << "FIT_EPSILON: Epsilon distribution for EE region " << jR << " loaded" << endl;
      }
  }

}



void  FitEpsilonPlot::deleteEpsilonPlot(TH1F **h, int size)
{
    for(int jR=0; jR<size; jR++)
	  delete h[jR];

    delete h;
}


void FitEpsilonPlot::saveCoefficients() 
{
  /// output file
  std::string fileName = outputDir_  + "/" + outfilename_;
  outfile_ = new TFile(fileName.c_str(),"RECREATE");
  cout << "FIT_EPSILON: Saving Calibration Coefficients in " << fileName << " ... " << endl;;
  if(!outfile_) throw cms::Exception("WritingOutputFile") << "It was no possible to create output file " << fileName << "\n";
  outfile_->cd();

  // 2D calib map in the barrel
  TH2F* hmap_EB = new TH2F("calibMap_EB","EB calib coefficients: #eta on x, #phi on y",
			   2*EBDetId::MAX_IETA+1,-EBDetId::MAX_IETA-0.5,EBDetId::MAX_IETA+0.5,
			   EBDetId::MAX_IPHI, EBDetId::MIN_IPHI-0.5, EBDetId::MAX_IPHI+0.5 );
  hmap_EB->GetXaxis()->SetTitle("i#eta");
  hmap_EB->GetYaxis()->SetTitle("i#phi");
  TH2F* hmap_EEp = new TH2F("calibMap_EEp","EE+ calib coefficients",100,0.5,100.5,100,0.5,100.5);
  hmap_EEp->GetXaxis()->SetTitle("ix");
  hmap_EEp->GetYaxis()->SetTitle("iy");
  TH2F* hmap_EEm = new TH2F("calibMap_EEm","EE- calib coefficients",100,0.5,100.5,100,0.5,100.5);
  hmap_EEm->GetXaxis()->SetTitle("ix");
  hmap_EEm->GetYaxis()->SetTitle("iy");
  TH1F* hint = new TH1F("hint","Bin1: inRangeFit_ Bin2: finRangeFit_ Bin3: Barrel(0)/Endcap(1)",3,0.,3.);
  hint->SetBinContent(1,inRangeFit_);
  hint->SetBinContent(2,finRangeFit_);
  if( EEoEB_ == "Barrel" ) hint->SetBinContent(3,0);
  else                     hint->SetBinContent(3,1);
  hint->Write();

  /// filling Barrel Map
  for(int j=0; j<regionalCalibration_->getCalibMap()->getNRegionsEB(); ++j)  
    {
      std::vector<DetId> ids = regionalCalibration_->allDetIdsInEBRegion(j);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) {
	EBDetId ebid(*iid);
	int ix = ebid.ieta()+EBDetId::MAX_IETA+1;

	float coeffValue = regionalCalibration_->getCalibMap()->coeff(*iid) > 0. ? regionalCalibration_->getCalibMap()->coeff(*iid) : 1.;
	hmap_EB->SetBinContent( ix, ebid.iphi(), coeffValue );
      } // loop over DetId in regions
    }
  hmap_EB->SetMinimum(0.9);
  hmap_EB->SetStats(false);
  hmap_EB->Write();

  for(int jR=0; jR < regionalCalibration_->getCalibMap()->getNRegionsEE(); jR++)
    {
      std::vector<DetId> ids =  regionalCalibration_->allDetIdsInEERegion(jR);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
	{ 
	  EEDetId eeid(*iid);
	  float coeffValue =  regionalCalibration_->getCalibMap()->coeff(*iid) > 0. ?  regionalCalibration_->getCalibMap()->coeff(*iid) : 1.;

	  if(eeid.positiveZ())
	    hmap_EEp->Fill(eeid.ix(), eeid.iy(), coeffValue); 
	  else 
	    hmap_EEm->Fill(eeid.ix(), eeid.iy(), coeffValue);
	}
    }

  hmap_EEp->SetMinimum(0.9);
  hmap_EEp->SetStats(false);
  hmap_EEp->Write();

  hmap_EEm->SetMinimum(0.9);
  hmap_EEm->SetStats(false);
  hmap_EEm->Write();

  /*------------- TTREE --------------*/

  uint32_t   rawId;
  int        hashedIndex;
  int        ieta;
  int        iphi;
  int        iSM;
  int        iMod;
  int        iTT;
  int        iTTeta;
  int        iTTphi;
  int        iter = currentIteration_;
  float      regCoeff;
  float      Signal;//#
  float      Backgr; 
  float      Chisqu; 
  float      Ndof; 
  float      fit_mean;
  float      fit_mean_err;
  float      fit_sigma;
  float      fit_Snorm;
  float      fit_b0;
  float      fit_b1;    
  float      fit_b2;    
  float      fit_b3;    
  float      fit_Bnorm; 
  /// endcap variables
  int ix;
  int iy;
  int zside;
  int sc; 
  int isc;
  int ic;
  int iquadrant;

  TTree* treeEB = new TTree("calibEB","Tree of EB Inter-calibration constants");
  TTree* treeEE = new TTree("calibEE","Tree of EE Inter-calibration constants");


  /// barrel
  treeEB->Branch("rawId",&rawId,"rawId/i");
  treeEB->Branch("hashedIndex",&hashedIndex,"hashedIndex/I");
  treeEB->Branch("ieta",&ieta,"ieta/I");
  treeEB->Branch("iphi",&iphi,"iphi/I");
  treeEB->Branch("iSM",&iSM,"iSM/I");
  treeEB->Branch("iMod",&iMod,"iMod/I");
  treeEB->Branch("iTT",&iTT,"iTT/I");
  treeEB->Branch("iTTeta",&iTTeta,"iTTeta/I");
  treeEB->Branch("iTTphi",&iTTphi,"iTTphi/I");
  treeEB->Branch("iter",&iter,"iter/I");
  treeEB->Branch("coeff",&regCoeff,"coeff/F");
  treeEB->Branch("Signal",&Signal,"Signal/F");//#
  treeEB->Branch("Backgr",&Backgr,"Backgr/F");
  treeEB->Branch("Chisqu",&Chisqu,"Chisqu/F");
  treeEB->Branch("Ndof",&Ndof,"Ndof/F");
  treeEB->Branch("fit_mean",&fit_mean,"fit_mean/F");
  treeEB->Branch("fit_mean_err",&fit_mean_err,"fit_mean_err/F");
  treeEB->Branch("fit_sigma",&fit_sigma,"fit_sigma/F");
  treeEB->Branch("fit_Snorm",&fit_Snorm,"fit_Snorm/F");
  treeEB->Branch("fit_b0",&fit_b0,"fit_b0/F");
  treeEB->Branch("fit_b1",&fit_b1,"fit_b1/F");
  treeEB->Branch("fit_b2",&fit_b2,"fit_b2/F");
  treeEB->Branch("fit_b3",&fit_b3,"fit_b3/F");
  treeEB->Branch("fit_Bnorm",&fit_Bnorm,"fit_Bnorm/F");

  /// endcap
  treeEE->Branch("ix",&ix,"ix/I");
  treeEE->Branch("iy",&iy,"iy/I");
  treeEE->Branch("zside",&zside,"zside/I");
  treeEE->Branch("sc",&sc,"sc/I");
  treeEE->Branch("isc",&isc,"isc/I");
  treeEE->Branch("ic",&ic,"ic/I");
  treeEE->Branch("iquadrant",&iquadrant,"iquadrant/I");
  treeEE->Branch("hashedIndex",&hashedIndex,"hashedIndex/I");
  treeEE->Branch("iter",&iter,"iter/I");
  treeEE->Branch("coeff",&regCoeff,"coeff/F");
  treeEE->Branch("Signal",&Signal,"Signal/F");//#
  treeEE->Branch("Backgr",&Backgr,"Backgr/F");
  treeEE->Branch("Chisqu",&Chisqu,"Chisqu/F");
  treeEE->Branch("Ndof",&Ndof,"Ndof/F");
  treeEE->Branch("fit_mean",&fit_mean,"fit_mean/F");
  treeEE->Branch("fit_mean_err",&fit_mean_err,"fit_mean_err/F");
  treeEE->Branch("fit_sigma",&fit_sigma,"fit_sigma/F");
  treeEE->Branch("fit_Snorm",&fit_Snorm,"fit_Snorm/F");
  treeEE->Branch("fit_b0",&fit_b0,"fit_b0/F");
  treeEE->Branch("fit_b1",&fit_b1,"fit_b1/F");
  treeEE->Branch("fit_b2",&fit_b2,"fit_b2/F");
  treeEE->Branch("fit_b3",&fit_b3,"fit_b3/F");
  treeEE->Branch("fit_Bnorm",&fit_Bnorm,"fit_Bnorm/F");


  for(int iR=0; iR < regionalCalibration_->getCalibMap()->getNRegionsEB(); ++iR)  {
    std::vector<DetId> ids = regionalCalibration_->allDetIdsInEBRegion(iR);
    for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) {
      EBDetId ebid(*iid);
      hashedIndex = ebid.hashedIndex();
      ieta = ebid.ieta();
      iphi = ebid.iphi();
      iSM = ebid.ism();
      iMod = ebid.im();
      iTT  = ebid.tower().hashedIndex();
      iTTeta = ebid.tower_ieta();
      iTTphi = ebid.tower_iphi();
      Signal = EBmap_Signal[ebid.hashedIndex()];//#
      Backgr = EBmap_Backgr[ebid.hashedIndex()];
      Chisqu = EBmap_Chisqu[ebid.hashedIndex()];
      Ndof = EBmap_ndof[ebid.hashedIndex()];
      fit_mean     = EBmap_mean[ebid.hashedIndex()];
      fit_mean_err = EBmap_mean_err[ebid.hashedIndex()];
      fit_sigma  = EBmap_sigma[ebid.hashedIndex()];
      fit_Snorm  = EBmap_Snorm[ebid.hashedIndex()];
      fit_b0     = EBmap_b0[ebid.hashedIndex()];
      fit_b1     = EBmap_b1[ebid.hashedIndex()];
      fit_b2     = EBmap_b2[ebid.hashedIndex()];
      fit_b3     = EBmap_b3[ebid.hashedIndex()];
      fit_Bnorm  = EBmap_Bnorm[ebid.hashedIndex()];

      regCoeff = regionalCalibration_->getCalibMap()->coeff(*iid);

      treeEB->Fill();
    } // loop over DetId in regions
  } // loop over regions

  for(int jR=0; jR < regionalCalibration_->getCalibMap()->getNRegionsEE() ; jR++)
    {
      std::vector<DetId> ids = regionalCalibration_->allDetIdsInEERegion(jR);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
	{ 
	  EEDetId eeid(*iid);
	  ix = eeid.ix();
	  iy = eeid.iy();
	  zside = eeid.zside();
	  sc = eeid.sc();
	  isc = eeid.isc();
	  ic = eeid.ic();
	  iquadrant = eeid.iquadrant();
	  hashedIndex = eeid.hashedIndex();
	  regCoeff = regionalCalibration_->getCalibMap()->coeff(*iid);
	  Signal = EEmap_Signal[eeid.hashedIndex()];//#
	  Backgr = EEmap_Backgr[eeid.hashedIndex()];
	  Chisqu = EEmap_Chisqu[eeid.hashedIndex()];            
	  Ndof = EEmap_ndof[eeid.hashedIndex()];            
	  fit_mean     = EEmap_mean[eeid.hashedIndex()];
	  fit_mean_err = EEmap_mean_err[eeid.hashedIndex()];
	  fit_sigma  = EEmap_sigma[eeid.hashedIndex()];
	  fit_Snorm  = EEmap_Snorm[eeid.hashedIndex()];
	  fit_b0     = EEmap_b0[eeid.hashedIndex()];
	  fit_b1     = EEmap_b1[eeid.hashedIndex()];
	  fit_b2     = EEmap_b2[eeid.hashedIndex()];
	  fit_b3     = EEmap_b3[eeid.hashedIndex()];
	  fit_Bnorm  = EEmap_Bnorm[eeid.hashedIndex()];

	  treeEE->Fill();
	}
    }

  treeEB->Write();
  treeEE->Write();

  outfile_->Write();
  outfile_->Close();
  cout << "FIT_EPSILON:  done" << endl;

}

//==========================

void FitEpsilonPlot::saveCoefficientsEoverEtrue(const bool isSecondGenPhoton = false) 
{

  // important, if using the second photon the output file is updated, so the call with isSecondGenPhoton = true should be made as the second one
  // otherwise, based on the current implementation, at the time you open the file for the first photon the file would be overwritten due to RECREATE mode

  /// output file
  std::string fileName = outputDir_  + "/" + outfilename_;
  if (isSecondGenPhoton) outfile_ = new TFile(fileName.c_str(),"UPDATE");
  else                   outfile_ = new TFile(fileName.c_str(),"RECREATE");
  cout << "FIT_EPSILON: Saving Calibration Coefficients in " << fileName << " ... " << endl;;
  if(!outfile_) throw cms::Exception("WritingOutputFile") << "It was no possible to create output file " << fileName << "\n";
  outfile_->cd();

  // 2D calib map in the barrel
  TH2F* hmap_EB = new TH2F((isSecondGenPhoton ? "calibMap_EB" : "calibMap_EB_g2"),"EB calib coefficients: #eta on x, #phi on y",
			   2*EBDetId::MAX_IETA+1,-EBDetId::MAX_IETA-0.5,EBDetId::MAX_IETA+0.5,
			   EBDetId::MAX_IPHI, EBDetId::MIN_IPHI-0.5, EBDetId::MAX_IPHI+0.5 );
  hmap_EB->GetXaxis()->SetTitle("i#eta");
  hmap_EB->GetYaxis()->SetTitle("i#phi");
  TH2F* hmap_EEp = new TH2F((isSecondGenPhoton ? "calibMap_EEp" : "calibMap_EEp_g2"),"EE+ calib coefficients",100,0.5,100.5,100,0.5,100.5);
  hmap_EEp->GetXaxis()->SetTitle("ix");
  hmap_EEp->GetYaxis()->SetTitle("iy");
  TH2F* hmap_EEm = new TH2F((isSecondGenPhoton ? "calibMap_EEm" : "calibMap_EEm_g2"),"EE- calib coefficients",100,0.5,100.5,100,0.5,100.5);
  hmap_EEm->GetXaxis()->SetTitle("ix");
  hmap_EEm->GetYaxis()->SetTitle("iy");
  TH1F* hint = new TH1F("hint","Bin1: inRangeFit_ Bin2: finRangeFit_ Bin3: Barrel(0)/Endcap(1)",3,0.,3.);
  hint->SetBinContent(1,inRangeFit_);
  hint->SetBinContent(2,finRangeFit_);
  if( EEoEB_ == "Barrel" ) hint->SetBinContent(3,0);
  else                     hint->SetBinContent(3,1);
  hint->Write();

  EcalRegionalCalibrationBase* regCalibToUse = (isSecondGenPhoton) regionalCalibration_g2_ : regionalCalibration_;

  /// filling Barrel Map
  for(int j=0; j<regCalibToUse->getCalibMap()->getNRegionsEB(); ++j)  
    {
      std::vector<DetId> ids = regCalibToUse->allDetIdsInEBRegion(j);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) {
	EBDetId ebid(*iid);
	int ix = ebid.ieta()+EBDetId::MAX_IETA+1;

	float coeffValue = regCalibToUse->getCalibMap()->coeff(*iid) > 0. ? regCalibToUse->getCalibMap()->coeff(*iid) : 1.;
	hmap_EB->SetBinContent( ix, ebid.iphi(), coeffValue );
      } // loop over DetId in regions
    }
  hmap_EB->SetMinimum(0.9);
  hmap_EB->SetStats(false);
  hmap_EB->Write();

  for(int jR=0; jR < regCalibToUse->getCalibMap()->getNRegionsEE(); jR++)
    {
      std::vector<DetId> ids = regCalibToUse->allDetIdsInEERegion(jR);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
	{ 
	  EEDetId eeid(*iid);
	  float coeffValue =  regCalibToUse->getCalibMap()->coeff(*iid) > 0. ?  regCalibToUse->getCalibMap()->coeff(*iid) : 1.;

	  if(eeid.positiveZ())
	    hmap_EEp->Fill(eeid.ix(), eeid.iy(), coeffValue); 
	  else 
	    hmap_EEm->Fill(eeid.ix(), eeid.iy(), coeffValue);
	}
    }

  hmap_EEp->SetMinimum(0.9);
  hmap_EEp->SetStats(false);
  hmap_EEp->Write();

  hmap_EEm->SetMinimum(0.9);
  hmap_EEm->SetStats(false);
  hmap_EEm->Write();

  /*------------- TTREE --------------*/
  uint32_t   rawId;
  int        hashedIndex;
  int        ieta;
  int        iphi;
  int        iSM;
  int        iMod;
  int        iTT;
  int        iTTeta;
  int        iTTphi;
  int        iter = currentIteration_;
  float      regCoeff;
  // float      Signal;//#
  // float      Backgr; 
  float      Chisqu; 
  float      Ndof; 
  float      fit_mean;
  float      fit_mean_err;
  float      fit_sigma;
  // float      fit_Snorm;
  // float      fit_b0;
  // float      fit_b1;    
  // float      fit_b2;    
  // float      fit_b3;    
  // float      fit_Bnorm; 
  /// endcap variables
  int ix;
  int iy;
  int zside;
  int sc; 
  int isc;
  int ic;
  int iquadrant;

  TTree* treeEB = new TTree((isSecondGenPhoton ? "calibEB" : "calibEB_g2"),"Tree of EB Inter-calibration constants");
  TTree* treeEE = new TTree((isSecondGenPhoton ? "calibEE" : "calibEE_g2"),"Tree of EE Inter-calibration constants");

  /// barrel
  treeEB->Branch("rawId",&rawId,"rawId/i");
  treeEB->Branch("hashedIndex",&hashedIndex,"hashedIndex/I");
  treeEB->Branch("ieta",&ieta,"ieta/I");
  treeEB->Branch("iphi",&iphi,"iphi/I");
  treeEB->Branch("iSM",&iSM,"iSM/I");
  treeEB->Branch("iMod",&iMod,"iMod/I");
  treeEB->Branch("iTT",&iTT,"iTT/I");
  treeEB->Branch("iTTeta",&iTTeta,"iTTeta/I");
  treeEB->Branch("iTTphi",&iTTphi,"iTTphi/I");
  treeEB->Branch("iter",&iter,"iter/I");
  treeEB->Branch("coeff",&regCoeff,"coeff/F");
  // treeEB->Branch("Signal",&Signal,"Signal/F");//#
  // treeEB->Branch("Backgr",&Backgr,"Backgr/F");
  treeEB->Branch("Chisqu",&Chisqu,"Chisqu/F");
  treeEB->Branch("Ndof",&Ndof,"Ndof/F");
  treeEB->Branch("fit_mean",&fit_mean,"fit_mean/F");
  treeEB->Branch("fit_mean_err",&fit_mean_err,"fit_mean_err/F");
  treeEB->Branch("fit_sigma",&fit_sigma,"fit_sigma/F");
  // treeEB->Branch("fit_Snorm",&fit_Snorm,"fit_Snorm/F");
  // treeEB->Branch("fit_b0",&fit_b0,"fit_b0/F");
  // treeEB->Branch("fit_b1",&fit_b1,"fit_b1/F");
  // treeEB->Branch("fit_b2",&fit_b2,"fit_b2/F");
  // treeEB->Branch("fit_b3",&fit_b3,"fit_b3/F");
  // treeEB->Branch("fit_Bnorm",&fit_Bnorm,"fit_Bnorm/F");

  /// endcap
  treeEE->Branch("ix",&ix,"ix/I");
  treeEE->Branch("iy",&iy,"iy/I");
  treeEE->Branch("zside",&zside,"zside/I");
  treeEE->Branch("sc",&sc,"sc/I");
  treeEE->Branch("isc",&isc,"isc/I");
  treeEE->Branch("ic",&ic,"ic/I");
  treeEE->Branch("iquadrant",&iquadrant,"iquadrant/I");
  treeEE->Branch("hashedIndex",&hashedIndex,"hashedIndex/I");
  treeEE->Branch("iter",&iter,"iter/I");
  treeEE->Branch("coeff",&regCoeff,"coeff/F");
  // treeEE->Branch("Signal",&Signal,"Signal/F");//#
  // treeEE->Branch("Backgr",&Backgr,"Backgr/F");
  treeEE->Branch("Chisqu",&Chisqu,"Chisqu/F");
  treeEE->Branch("Ndof",&Ndof,"Ndof/F");
  treeEE->Branch("fit_mean",&fit_mean,"fit_mean/F");
  treeEE->Branch("fit_mean_err",&fit_mean_err,"fit_mean_err/F");
  treeEE->Branch("fit_sigma",&fit_sigma,"fit_sigma/F");
  // treeEE->Branch("fit_Snorm",&fit_Snorm,"fit_Snorm/F");
  // treeEE->Branch("fit_b0",&fit_b0,"fit_b0/F");
  // treeEE->Branch("fit_b1",&fit_b1,"fit_b1/F");
  // treeEE->Branch("fit_b2",&fit_b2,"fit_b2/F");
  // treeEE->Branch("fit_b3",&fit_b3,"fit_b3/F");
  // treeEE->Branch("fit_Bnorm",&fit_Bnorm,"fit_Bnorm/F");

  for(int iR=0; iR < regCalibToUse->getCalibMap()->getNRegionsEB(); ++iR)  {
    std::vector<DetId> ids = regCalibToUse->allDetIdsInEBRegion(iR);
    for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) {
      EBDetId ebid(*iid);
      hashedIndex = ebid.hashedIndex();
      ieta = ebid.ieta();
      iphi = ebid.iphi();
      iSM = ebid.ism();
      iMod = ebid.im();
      iTT  = ebid.tower().hashedIndex();
      iTTeta = ebid.tower_ieta();
      iTTphi = ebid.tower_iphi();
      // Signal = EBmap_Signal[ebid.hashedIndex()];//#
      // Backgr = EBmap_Backgr[ebid.hashedIndex()];
      Chisqu = EBmap_Chisqu[ebid.hashedIndex()];
      Ndof = EBmap_ndof[ebid.hashedIndex()];
      fit_mean     = EBmap_mean[ebid.hashedIndex()];
      fit_mean_err = EBmap_mean_err[ebid.hashedIndex()];
      fit_sigma  = EBmap_sigma[ebid.hashedIndex()];
      // fit_Snorm  = EBmap_Snorm[ebid.hashedIndex()];
      // fit_b0     = EBmap_b0[ebid.hashedIndex()];
      // fit_b1     = EBmap_b1[ebid.hashedIndex()];
      // fit_b2     = EBmap_b2[ebid.hashedIndex()];
      // fit_b3     = EBmap_b3[ebid.hashedIndex()];
      // fit_Bnorm  = EBmap_Bnorm[ebid.hashedIndex()];

      regCoeff = regCalibToUse->getCalibMap()->coeff(*iid);

      treeEB->Fill();
    } // loop over DetId in regions
  } // loop over regions

  for(int jR=0; jR < regCalibToUse->getCalibMap()->getNRegionsEE() ; jR++)
    {
      std::vector<DetId> ids = regCalibToUse->allDetIdsInEERegion(jR);
      for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
	{ 
	  EEDetId eeid(*iid);
	  ix = eeid.ix();
	  iy = eeid.iy();
	  zside = eeid.zside();
	  sc = eeid.sc();
	  isc = eeid.isc();
	  ic = eeid.ic();
	  iquadrant = eeid.iquadrant();
	  hashedIndex = eeid.hashedIndex();
	  regCoeff = regCalibToUse->getCalibMap()->coeff(*iid);
	  // Signal = EEmap_Signal[eeid.hashedIndex()];//#
	  // Backgr = EEmap_Backgr[eeid.hashedIndex()];
	  Chisqu = EEmap_Chisqu[eeid.hashedIndex()];            
	  Ndof = EEmap_ndof[eeid.hashedIndex()];            
	  fit_mean     = EEmap_mean[eeid.hashedIndex()];
	  fit_mean_err = EEmap_mean_err[eeid.hashedIndex()];
	  fit_sigma  = EEmap_sigma[eeid.hashedIndex()];
	  // fit_Snorm  = EEmap_Snorm[eeid.hashedIndex()];
	  // fit_b0     = EEmap_b0[eeid.hashedIndex()];
	  // fit_b1     = EEmap_b1[eeid.hashedIndex()];
	  // fit_b2     = EEmap_b2[eeid.hashedIndex()];
	  // fit_b3     = EEmap_b3[eeid.hashedIndex()];
	  // fit_Bnorm  = EEmap_Bnorm[eeid.hashedIndex()];

	  treeEE->Fill();
	}
    }

  treeEB->Write();
  treeEE->Write();

  outfile_->Write();
  outfile_->Close();
  cout << "FIT_EPSILON:  done" << endl;

}


// ------------ method called for each event  ------------

void FitEpsilonPlot::analyze(const edm::Event& iEvent, const edm::EventSetup& iSetup)
{

    TF1 ffit("gausa","gaus(0)+[3]*x+[4]",-0.5,0.5);
    ffit.SetParameters(100,0,0.1);
    ffit.SetParNames("Constant","Mean_value","Sigma","a","b");

    ffit.SetParLimits(3,-500,500);
    ffit.SetParLimits(2,0.05,0.22);

    cout << "FIT_EPSILON: About to fit epsilon distributions" << endl; 

    /// compute average weight, eps, and update calib constant
    if( (EEoEB_ == "Barrel") && (Barrel_orEndcap_=="ONLY_BARREL" || Barrel_orEndcap_=="ALL_PLEASE" ) ){
	  for(uint32_t j= (uint32_t)inRangeFit_; j <= (uint32_t)finRangeFit_ && j < (uint32_t)regionalCalibration_->getCalibMap()->getNRegionsEB(); ++j)  
	  {
		cout<<"FIT_EPSILON: Fitting EB Cristal--> "<<j<<endl;

		if(!(j%1000)) cout << "FIT_EPSILON: fitting EB region " << j << endl;

		float mean = 0.;
		float mean_g2 = 0.; // used only for E/Etrue with MC

		if (isEoverEtrue_) {

		  // first photon 
		  int iMin = EoverEtrue_g1_EB_h[j]->GetXaxis()->FindFixBin(0.6); 
		  int iMax = EoverEtrue_g1_EB_h[j]->GetXaxis()->FindFixBin(1.1);
		  double integral = EoverEtrue_g1_EB_h[j]->Integral(iMin, iMax);  

		  if(integral>100.) {

		    TFitResultPtr fitres = FitEoverEtruePeak( EoverEtrue_g1_EB_h[j], j, Pi0EB, false);
		    mean = fitres->Parameter(2);
		    float r2 = mean;
		    r2 = r2*r2;
		    mean = 0.5 * ( r2 - 1. );  // keep as for mass: we have IC = 1/(1+mean) = 2 /(r^2 +1), if r2 < 1 then IC > 1
		    
		  } else mean = 0.;

		  // second photon 
		  iMin = EoverEtrue_g2_EB_h[j]->GetXaxis()->FindFixBin(0.6); 
		  iMax = EoverEtrue_g2_EB_h[j]->GetXaxis()->FindFixBin(1.1);
		  integral = EoverEtrue_g2_EB_h[j]->Integral(iMin, iMax);  

		  if(integral>100.) {

		    TFitResultPtr fitres = FitEoverEtruePeak( EoverEtrue_g2_EB_h[j], j, Pi0EB, false);
		    mean_g2 = fitres->Parameter(2);
		    float r2 = mean_g2;
		    r2 = r2*r2;
		    mean_g2 = 0.5 * ( r2 - 1. );  // keep as for mass: we have IC = 1/(1+mean) = 2 /(r^2 +1), if r2 < 1 then IC > 1
		    
		  } else mean_g2 = 0.;

		  
		} else {
		  
		  if(!useMassInsteadOfEpsilon_ && epsilon_EB_h[j]->Integral(epsilon_EB_h[j]->GetNbinsX()*(1./6.),epsilon_EB_h[j]->GetNbinsX()*0.5) > 20) 
		    {

		      double Max = 0.;
		      double Min = -0.5, bin = 0.0125;
		      Max = Min+(bin*(double)epsilon_EB_h[j]->GetMaximumBin());
		      double Bound1 = -0.15, Bound2 = 0.25;
		      if ( fabs(Max+Bound1) > 0.24  ){ Bound1 = -0.1;}
		      if ( Max+Bound2 > 0.34  ){ Bound2 = 0.15;}
		      if ( fabs(Max+Bound1) > 0.24  ){ Bound1 = -0.075;}
		      if ( Max+Bound2 > 0.34  ){ Bound2 = 0.1;}
		      if ( fabs(Max+Bound1) > 0.24  ){ Bound1 = -0.03;}
		      if ( Max+Bound2 > 0.34  ){ Bound2 = 0.05;}
		      if ( fabs(Max+Bound1) > 0.24  ){ Bound1 = -0.009;}
		      if ( Max+Bound2 > 0.34  ){ Bound2 = 0.01;}

		      epsilon_EB_h[j]->Fit(&ffit,"qB","", Max+Bound1,Max+Bound2);
		      if(ffit.GetNDF() != 0) {
			double chi2 = ( ffit.GetChisquare()/ffit.GetNDF() );

			if ( chi2  > 11 ){
			  ffit.SetParLimits(2,0.05,0.15);
			  ffit.SetParameters(100,0,0.1);
			  epsilon_EB_h[j]->Fit(&ffit,"qB","", Max+Bound1,Max+Bound2);
			  chi2 = (ffit.GetChisquare()/ffit.GetNDF());
			  if ( chi2  < 11 ){   cout<<"Saved 1 Level!!"<<endl;  }
			  else{
			    ffit.SetParameters(100,0,0.1);
			    ffit.SetParLimits(2,0.05,0.1);
			    epsilon_EB_h[j]->Fit(&ffit,"qB","",  Max+Bound1,Max+Bound2);
			    chi2 = (ffit.GetChisquare()/ffit.GetNDF());
			    if ( chi2  < 11 ){ cout<<"Saved 2 Level!!"<<endl; }
			    else{ cout<<"DAMN: High Chi square..."<<endl; }
			  }
			}
		      }
		      else cout<<"DAMN: NDF == 0"<<endl;
		      mean = ffit.GetParameter(1);
		    }
		  else if(useMassInsteadOfEpsilon_)
		    {
		      int iMin = epsilon_EB_h[j]->GetXaxis()->FindFixBin(Are_pi0_? 0.08:0.4 ); 
		      int iMax = epsilon_EB_h[j]->GetXaxis()->FindFixBin(Are_pi0_? 0.18:0.65 );
		      double integral = epsilon_EB_h[j]->Integral(iMin, iMax);  

		      if(integral>60.) {

			Pi0FitResult fitres = FitMassPeakRooFit( epsilon_EB_h[j], Are_pi0_? 0.08:0.4, Are_pi0_? 0.21:0.65, j, 1, Pi0EB, 0, isNot_2010_); //0.05-0.3
			RooRealVar* mean_fitresult = (RooRealVar*)(((fitres.res)->floatParsFinal()).find("mean"));
			mean = mean_fitresult->getVal();

			float r2 = mean/(Are_pi0_? PI0MASS:ETAMASS);
			r2 = r2*r2;
			//cout<<"EBMEAN::"<<j<<":"<<mean<<" Saved if: "<<fitres.SoB<<">(isNot_2010_ ? 0.04:0.1) "<<(fitres.chi2/fitres.dof)<<" < 0.2 "<<fabs(mean-0.15)<<" >0.0000001) "<<endl;
			//if( fitres.SoB>(isNot_2010_ ? 0.04:0.1) && (fitres.chi2/fitres.dof)< 0.5 && fabs(mean-0.15)>0.0000001) mean = 0.5 * ( r2 - 1. );
			//if( fitres.chi2 < 5 && fabs(mean-(Are_pi0_? upper_bound_pi0mass_EB:upper_bound_etamass_EB))>0.0000001) mean = 0.5 * ( r2 - 1. );
			if( fabs(mean-(Are_pi0_? upper_bound_pi0mass_EB:upper_bound_etamass_EB)) > 0.0000001 )
			  mean = 0.5 * ( r2 - 1. );
			else 
			  mean = 0.;
		      }
		      else{
			mean = 0.;
		      }
		    }

		}

		std::vector<DetId> ids = regionalCalibration_->allDetIdsInEBRegion(j);
		for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
		{
		    regionalCalibration_->getCalibMap()->coeff(*iid) *= (mean==0.) ? 1. : 1./(1.+mean);
		} // loop over DetId in regions

		// now loop on second photon if doing E/Etrue
		if (isEoverEtrue_) {
		  ids = regionalCalibration_g2_->allDetIdsInEBRegion(j);
		  for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
		    {
		      regionalCalibration_g2_->getCalibMap()->coeff(*iid) *= (mean_g2==0.) ? 1. : 1./(1.+mean_g2);
		    } // loop over DetId in regions
		}

	  } // loop over regions
    }// if you have to fit barrel

    /// loop over EE crystals
    if( (EEoEB_ == "Endcap") && (Barrel_orEndcap_=="ONLY_ENDCAP" || Barrel_orEndcap_=="ALL_PLEASE" ) ){
	  for(int jR = inRangeFit_; jR <=finRangeFit_ && jR < regionalCalibration_->getCalibMap()->getNRegionsEE(); jR++)
	  {
		cout << "FIT_EPSILON: Fitting EE Cristal--> " << jR << endl;
		if(!(jR%1000))
		    cout << "FIT_EPSILON: fitting EE region " << jR << endl;

		float mean = 0.;
		float mean_g2 = 0.; // used only for E/Etrue with MC

		if (isEoverEtrue_) {
		  
		  int iMin = EoverEtrue_g1_EE_h[jR]->GetXaxis()->FindFixBin(0.6); 
		  int iMax = EoverEtrue_g1_EE_h[jR]->GetXaxis()->FindFixBin(1.1);
		  double integral = EoverEtrue_g1_EE_h[jR]->Integral(iMin, iMax);  

		  if(integral>100.) {

		    TFitResultPtr fitres = FitEoverEtruePeak( EoverEtrue_g1_EE_h[jR], jR, Pi0EE, false);
		    mean = fitres->Parameter(2);
		    float r2 = mean;
		    r2 = r2*r2;
		    mean = 0.5 * ( r2 - 1. );  // keep as for mass: we have IC = 1/(1+mean) = 2 /(r^2 +1), if r2 < 1 then IC > 1
		    
		  } else mean = 0.;

		  iMin = EoverEtrue_g2_EE_h[jR]->GetXaxis()->FindFixBin(0.6); 
		  iMax = EoverEtrue_g2_EE_h[jR]->GetXaxis()->FindFixBin(1.1);
		  integral = EoverEtrue_g2_EE_h[jR]->Integral(iMin, iMax);  

		  if(integral>100.) {

		    TFitResultPtr fitres = FitEoverEtruePeak( EoverEtrue_g2_EE_h[jR], jR, Pi0EE, false);
		    mean_g2 = fitres->Parameter(2);
		    float r2 = mean_g2;
		    r2 = r2*r2;
		    mean_g2 = 0.5 * ( r2 - 1. );  // keep as for mass: we have IC = 1/(1+mean) = 2 /(r^2 +1), if r2 < 1 then IC > 1
		    
		  } else mean_g2 = 0.;

		  
		} else {
		
		  if(!useMassInsteadOfEpsilon_ && epsilon_EE_h[jR]->Integral(epsilon_EE_h[jR]->GetNbinsX()*(1./6.),epsilon_EE_h[jR]->GetNbinsX()*0.5) > 20) 
		    {
		      TF1 *ffit = new TF1("gausa","gaus(0)+[3]*x+[4]",-0.5,0.5);
		      ffit->SetParameters(100,0,0.1);
		      ffit->SetParNames("Constant","Mean_value","Sigma","a","b");

		      ffit->SetParLimits(0,0.,epsilon_EE_h[jR]->GetEntries()*1.1);
		      ffit->SetParLimits(3,-500,500);
		      ffit->SetParLimits(2,0.05,0.3);

		      double Max = 0.;
		      double Min = -0.5, bin = 0.0125;
		      Max = Min+(bin*(double)epsilon_EE_h[jR]->GetMaximumBin());
		      double Bound1 = -0.35, Bound2 = 0.35;
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.3;}
		      if ( Max+Bound2 > 0.48  ){ Bound2 = 0.3;}
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.25;}
		      if ( Max+Bound2 > 0.48  ){ Bound2 = 0.2;}
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.2;}
		      if ( Max+Bound2 > 0.48  ){ Bound2 = 0.15;}
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.15;}
		      if ( Max+Bound2 > 0.48  ){ Bound2 = 0.1;}
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.1;}
		      if ( fabs(Max+Bound1) > 0.38  ){ Bound1 = -0.05;}
		      //@@IterativeFit(epsilon_EE_h[jR], *ffit);
		      //@@mean = ffit.GetParameter(1); 
		      epsilon_EE_h[jR]->Fit(ffit,"qB","", Max+Bound1,Max+Bound2);

		      if(ffit->GetNDF() != 0) {
			double chi2 = ( ffit->GetChisquare()/ffit->GetNDF() );
			if(chi2 > 11  ) { cout<<"DAMN:(EE) High Chi square..."<<endl; }
		      }
		      else cout<<"DAMN: NDF == 0"<<endl;
		      mean = ffit->GetParameter(1);
		    }
		  else if(useMassInsteadOfEpsilon_)
		    {
		      int iMin = epsilon_EE_h[jR]->GetXaxis()->FindFixBin(Are_pi0_? 0.08:0.4 ); 
		      int iMax = epsilon_EE_h[jR]->GetXaxis()->FindFixBin(Are_pi0_? 0.18:0.65 );
		      double integral = epsilon_EE_h[jR]->Integral(iMin, iMax);  

		      if(integral>70.)
			{
			  Pi0FitResult fitres = FitMassPeakRooFit( epsilon_EE_h[jR], Are_pi0_? 0.08:0.4, Are_pi0_? 0.21:0.65, jR, 1, Pi0EE, 0, isNot_2010_);//0.05-0.3
			  RooRealVar* mean_fitresult = (RooRealVar*)(((fitres.res)->floatParsFinal()).find("mean"));
			  mean = mean_fitresult->getVal();
			  float r2 = mean/(Are_pi0_? PI0MASS:ETAMASS);
			  r2 = r2*r2;
			  //cout<<"EEMEAN::"<<jR<<":"<<mean<<" Saved if: "<<fitres.SoB<<">0.3 "<<(fitres.chi2/fitres.dof)<<" < (isNot_2010_? 0.07:0.35) "<<fabs(mean-0.14)<<" >0.0000001) "<<endl;
			  //if( (fitres.chi2/fitres.dof)<0.3 && fitres.SoB>(isNot_2010_? 0.07:0.35) && fabs(mean-0.14)>0.0000001 ) mean = 0.5 * ( r2 - 1. );
			  //if( fitres.chi2 < 5 && fabs(mean-(Are_pi0_? upper_bound_pi0mass_EE:upper_bound_etamass_EE))>0.0000001 ) mean = 0.5 * ( r2 - 1. );
			  // do not use Chi2 for goodness of fit. If I have many events, then the chi2 will be huge because the model will not pass through all data points
			  // on the oter hand, if I have few events, the statistical uncertainty is large and the Chi2 tends to be little
			  // better not to use Chi2
			  if(fabs(mean-(Are_pi0_? upper_bound_pi0mass_EE:upper_bound_etamass_EE))>0.0000001 ) 
			    mean = 0.5 * ( r2 - 1. );
			  else
			    mean = 0.;
			}
		      else
			{
			  mean = 0.; 
			}
		    }

		}

		std::vector<DetId> ids = regionalCalibration_->allDetIdsInEERegion(jR);
		for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
		{
		    regionalCalibration_->getCalibMap()->coeff(*iid) *= (mean==0.) ? 1. : 1./(1.+mean);
		}

		// now loop on second photon if doing E/Etrue
		if (isEoverEtrue_) {
		  ids = regionalCalibration_g2_->allDetIdsInEERegion(j);
		  for(std::vector<DetId>::const_iterator iid = ids.begin(); iid != ids.end(); ++iid) 
		    {
		      regionalCalibration_g2_->getCalibMap()->coeff(*iid) *= (mean_g2==0.) ? 1. : 1./(1.+mean_g2);
		    } // loop over DetId in regions		  
		}

	  }//for EE
    }// if you have to fit Endcap

}



void FitEpsilonPlot::IterativeFit(TH1F* h, TF1 & ffit) 
{
    float chi2 = 1000.;
    const int iterMax = 10;

    h->Fit(&ffit,"q","",-0.4,0.4);

    float mean = (ffit.GetParameters())[1];
    float sigma = (ffit.GetParameters())[2];
    float xmin = mean-2.*sigma;
    float xmax = mean+2.*sigma;

    ffit.SetRange(xmin,xmax);

    double par[3] = { ffit.GetParameters()[0], mean, sigma };

    for(int iter=0; iter< iterMax && chi2>5.; ++iter) 
    {
	  ffit.SetParameters(par[0],par[1], par[2]);

	  h->Fit(&ffit,"q","",xmin,xmax);
	  par[0] = (ffit.GetParameters())[0];
	  par[1] = (ffit.GetParameters())[1];
	  par[2] = (ffit.GetParameters())[2];

	  if(ffit.GetNDF()!=0) {
		chi2 = ffit.GetChisquare()/ffit.GetNDF();
	  }

    }
    return;
}


//-----------------------------------------------------------------------------------

Pi0FitResult FitEpsilonPlot::FitMassPeakRooFit(TH1F* h, double xlo, double xhi,  uint32_t HistoIndex, int ngaus, FitMode mode, int niter, bool isNot_2010_) 
{
    //-----------------------------------------------------------------------------------

    std::stringstream ind;
    ind << (int) HistoIndex;
    TString nameHistofit = "Fit_n_" + ind.str() + Form("_attempt%d",niter);

    // add canvas to save rooplot on top (will save this in the file)
    TCanvas* canvas = new TCanvas((nameHistofit+Form("_c")).Data(),"",600,700);
    canvas->cd();
    canvas->SetTickx(1);
    canvas->SetTicky(1);
    canvas->cd();
    canvas->SetRightMargin(0.06);

    RooRealVar x("x","#gamma#gamma invariant mass",xlo, xhi, "GeV/c^2");

    RooDataHist dh("dh","#gamma#gamma invariant mass",RooArgList(x),h);

    RooRealVar mean("mean","#pi^{0} peak position", Are_pi0_? 0.13:0.52,  Are_pi0_? 0.105:0.5, Are_pi0_? upper_bound_pi0mass_EB:upper_bound_etamass_EB,"GeV/c^{2}");
    RooRealVar sigma("sigma","#pi^{0} core #sigma",0.011, 0.005,0.015,"GeV/c^{2}");


    if(mode==Pi0EE)  {
	  mean.setRange( Are_pi0_? 0.1:0.45, Are_pi0_? upper_bound_pi0mass_EE:upper_bound_etamass_EE);
	  mean.setVal(Are_pi0_? 0.13:0.55);
	  sigma.setRange(0.005, 0.020);
    }
    if(mode==Pi0EB && niter==1){
	  mean.setRange(Are_pi0_? 0.105:0.47, Are_pi0_? upper_bound_pi0mass_EB:upper_bound_etamass_EB);
	  sigma.setRange(0.003, 0.030);
    }

    //RooRealVar Nsig("Nsig","#pi^{0} yield",1000.,0.,1.e7);
    RooRealVar Nsig("Nsig","#pi^{0} yield",h->Integral()*0.15,0.,h->Integral()*10.0);
    //Nsig.setVal( h->GetSum()*0.1);

    RooGaussian gaus("gaus","Core Gaussian",x, mean,sigma);

    RooRealVar sigmaTail("sigmaTail","#pi^{0} tail #sigma",0.040, 0.020,0.065,"GeV/c^{2}");

    RooGaussian gaus2("gaus2","Tail Gaussian",x, mean,sigmaTail);

    RooRealVar fcore("fcore","f_{core}",0.9,0.,1.);
    RooAddPdf  signal("signal","signal model",RooArgList(gaus,gaus2),fcore);

    RooRealVar p0("p0","p0", 1000.,-1.e5,1.e5);
    RooRealVar p1("p1","p1", -3000.,-1.e5,1.e5);
    RooRealVar p2("p2","p2", 10000.,-1.e5,1.e5);
    RooRealVar p3("p3","p3", -10000.,-1.e5,1.e5);
    RooRealVar p4("p4","p4",-4000.,-1.e5,1.e5);
    RooRealVar p5("p5","p5", 5.,-1.e5,1.e5);
    RooRealVar p6("p6","p6", 6.,-1.e5,1.e5);

    RooRealVar cb0("cb0","cb0", 0.2, -1.,1.);
    RooRealVar cb1("cb1","cb1",-0.1, -1.,1.);
    RooRealVar cb2("cb2","cb2", 0.1,  -1.,1.);
    RooRealVar cb3("cb3","cb3",-0.1, -0.5,0.5);
    RooRealVar cb4("cb4","cb4", 0.1, -1.,1.);
    RooRealVar cb5("cb5","cb5", 0.1, -1.,1.);
    RooRealVar cb6("cb6","cb6", 0.3, -1.,1.);


    //RooChebychev bkg("bkg","bkg model", x, RooArgList(cb0,cb1,cb2) );
    //RooChebychev bkg("bkg","bkg model", x, RooArgList(cb0,cb1,cb2,cb3) );

    RooArgList cbpars(cb0,cb1,cb2);
    //if(mode==Pi0EE) cbpars.add( cb4);
    //if(mode==Pi0EE) cbpars.add( cb5);

    // try to use a second order polynomial, if the fit is bad add other terms
    // if you start with many terms, the fit creates strange curvy shapes trying to fit the statistical fluctuations
    // 2nd order means a curve with no change of concavity
    
    if(niter==1){
      cbpars.add( cb3);
    }
    if(niter==2){
      cb3.setRange(-1,1.);
      cb4.setRange(-0.3,0.3);
      cbpars.add( cb3);
      cbpars.add( cb4 );     
    }
    if(niter==3){
      cb3.setRange(-1,1.);
      cb4.setRange(-1,1);
      cb5.setRange(-0.5, 0.5);
      cbpars.add( cb3);
      cbpars.add( cb4 );
      cbpars.add( cb5 );
    }

    RooChebychev bkg("bkg","bkg model", x, cbpars );

    //RooPolynomial bkg("bkg","background model",x,RooArgList(p0,p1,p2,p3,p4,p5,p6) );
    //RooPolynomial bkg("bkg","background model",x,RooArgList(p0,p1,p2,p3) );

    //RooRealVar Nbkg("Nbkg","background yield",1.e3,0.,1.e8);
    RooRealVar Nbkg("Nbkg","background yield",h->Integral()*0.85,0.,h->Integral()*10.0);
    //Nbkg.setVal( h->GetSum()*0.8 );

    RooAbsPdf* model=0;

    RooAddPdf model1("model","sig+bkg",RooArgList(gaus,bkg),RooArgList(Nsig,Nbkg));
    RooAddPdf model2("model","sig+bkg",RooArgList(signal,bkg),RooArgList(Nsig,Nbkg));

    if(ngaus==1)      model = &model1;
    else if(ngaus==2) model = &model2;


    RooNLLVar nll("nll","log likelihood var",*model,dh, RooFit::Extended(true));
    //RooAbsReal * nll = model->createNLL(dh); //suggetsed way, taht should be the same

    // // original fit
    // // obsolete: see here --> https://root-forum.cern.ch/t/roominuit-and-roominimizer-difference/18230/8
    // // better to use RooMinimizer, but please read caveat below
    RooMinuit m(nll);
    m.setVerbose(kFALSE);
    //m.setVerbose(kTRUE);
    m.migrad();
    m.hesse();
    RooFitResult* res = m.save() ;

    // alternative fit (results are pretty much the same)
    // IMPORTANT, READ CAREFULLY: sometimes this method fails.
    // This happens because at the boundaries of the fit range the pdf goea slightly below 0 (so it is negative). The fitter tries to cope wth it and should tipically
    // manage to converge. However, I noticed that after few attemps (even though the default number of attemps should be several hundreds or thousands of times) 
    // the job crashes, and this seems to be a feature of cmssw, not of RooFit
    // The reason why the pdf gets negative could be due to the fact that, regardless the chosen fit range given by xlo and xhi, the actual fit range goes from the 
    // lower edge of the leftmost bin containing xlo to the upper edge of the rightmost one containing xhi, but then the fit tries to "pass" across the bin centers
    // Therefore, for a sharply rising (or falling) distribution, the pdf can become negative
    // The consequence is that there are large areas in the calibration map of related 2D plots that are white (because the fit there was not done succesfully)
    // The previous method using RooMinuit seems to be more robust, so I suggest we should use that one even though it is said to be obsolete
    // RooMinimizer mfit(nll);
    // mfit.setVerbose(kFALSE);
    // mfit.setPrintLevel(-1);
    // mfit.setStrategy(2);  // 0,1,2:  MINUIT strategies for dealing most efficiently with fast FCNs (0), expensive FCNs (2) and 'intermediate' FCNs (1)
    // //cout << "FIT_EPSILON: Minimize" << endl;
    // mfit.minimize("Minuit2","minimize");
    // //cout << "FIT_EPSILON: Minimize hesse " << endl;
    // mfit.minimize("Minuit2","hesse");
    // //cout<<"FIT_EPSILON: Estimate minos errors for all parameters"<<endl;
    // mfit.minos(RooArgSet(Nsig,Nbkg),mean);
    // RooFitResult* res = mfit.save() ;

    RooChi2Var chi2("chi2","chi2 var",*model,dh, true);
    // use only bins in fit range for ndof (dh is made with var x that already has the restricted range, but h is the full histogram)
    //int ndof = h->GetNbinsX() - res->floatParsFinal().getSize();
    int ndof = h->FindFixBin(xhi) - h->FindFixBin(xlo) - res->floatParsFinal().getSize();

    //compute S/B and chi2
    x.setRange("sobRange",mean.getVal()-3.*sigma.getVal(), mean.getVal()+3.*sigma.getVal());
    RooAbsReal* integralSig = gaus.createIntegral(x,NormSet(x),Range("sobRange"));

    RooAbsReal* integralBkg = bkg.createIntegral(x,NormSet(x),Range("sobRange"));

    float normSig = integralSig->getVal();
    float normBkg = integralBkg->getVal();

    Pi0FitResult pi0res; // this is the output value of this method
    pi0res.res = res;

    pi0res.S = normSig*Nsig.getVal();
    pi0res.Serr = normSig*Nsig.getError();

    pi0res.B = normBkg*Nbkg.getVal();
    pi0res.Berr = normBkg*Nbkg.getError();

    pi0res.SoB =  pi0res.S/pi0res.B;
    pi0res.SoBerr =  pi0res.SoB*sqrt( pow(pi0res.Serr/pi0res.S,2) + 
		pow(pi0res.Berr/pi0res.B,2) ) ;
    pi0res.dof = ndof;
    pi0res.nFitParam = res->floatParsFinal().getSize();


    RooPlot*  xframe = x.frame(h->GetNbinsX());
    //RooPlot*  xframe = x.frame(xlo, xhi);
    xframe->SetName((nameHistofit+Form("_rp")).Data());
    xframe->SetTitle(h->GetTitle());
    dh.plotOn(xframe, Name("data"));
    model->plotOn(xframe,Components(bkg),LineStyle(kDashed), LineColor(kRed));
    model->plotOn(xframe,Components(gaus),LineStyle(kDashed), LineColor(kGreen+1));
    model->plotOn(xframe, Name("model"));

    // TMAth::Prob() uses Chi2, not reduced Chi2, while xframe->chiSquare() returns the reduced Chi2
    pi0res.chi2 = xframe->chiSquare("model","data",pi0res.nFitParam) * pi0res.dof;
    pi0res.probchi2 = TMath::Prob(pi0res.chi2, ndof);

    xframe->Draw();

    cout << "FIT_EPSILON: Nsig: " << Nsig.getVal() 
	 << " nsig 3sig: " << normSig*Nsig.getVal()
	 << " nbkg 3sig: " << normBkg*Nbkg.getVal()
	 << " S/B: " << pi0res.SoB << " +/- " << pi0res.SoBerr
	 << " chi2: " << pi0res.chi2
	 << " chi2 reduced: " << pi0res.chi2 / pi0res.dof
	 << " DOF: " << pi0res.dof
	 << " N(fit.param.): " << pi0res.nFitParam
	 << " prob(chi2): " << pi0res.probchi2
	 << endl;

    if(mode==Pi0EB){
	  EBmap_Signal[HistoIndex]=pi0res.S;
	  EBmap_Backgr[HistoIndex]=pi0res.B;
	  EBmap_Chisqu[HistoIndex]=xframe->chiSquare();
	  EBmap_ndof[HistoIndex]=ndof;
	  EBmap_mean[HistoIndex]=mean.getVal();
	  EBmap_mean_err[HistoIndex]=mean.getError();
	  EBmap_sigma[HistoIndex]=sigma.getVal();
	  EBmap_Snorm[HistoIndex]=normSig;
	  EBmap_b0[HistoIndex]=cb0.getVal();
	  EBmap_b1[HistoIndex]=cb1.getVal();
	  EBmap_b2[HistoIndex]=cb2.getVal();
	  EBmap_b3[HistoIndex]=cb3.getVal();
	  EBmap_Bnorm[HistoIndex]=normBkg;
    }
    if(mode==Pi0EE){
	  EEmap_Signal[HistoIndex]=pi0res.S;
	  EEmap_Backgr[HistoIndex]=pi0res.B;
	  EEmap_Chisqu[HistoIndex]=xframe->chiSquare();
	  EEmap_ndof[HistoIndex]=ndof;
	  EEmap_mean[HistoIndex]=mean.getVal();
	  EEmap_mean_err[HistoIndex]=mean.getError();
	  EEmap_sigma[HistoIndex]=sigma.getVal();
	  EEmap_Snorm[HistoIndex]=normSig;
	  EEmap_b0[HistoIndex]=cb0.getVal();
	  EEmap_b1[HistoIndex]=cb1.getVal();
	  EEmap_b2[HistoIndex]=cb2.getVal();
	  EEmap_b3[HistoIndex]=cb3.getVal();
	  EEmap_Bnorm[HistoIndex]=normBkg;
    }

    TLatex lat;
    std::string line = "";
    lat.SetNDC();
    lat.SetTextSize(0.040);
    lat.SetTextColor(1);

    float xmin(0.58), yhi(0.80), ypass(0.05);
    if(mode==EtaEB) yhi=0.30;
    line = Form("Yield: %.0f #pm %.0f", Nsig.getVal(), Nsig.getError() );
    lat.DrawLatex(xmin,yhi, line.c_str());

    line = Form("m_{#gamma#gamma}: %.2f #pm %.2f", mean.getVal()*1000., mean.getError()*1000. );
    lat.DrawLatex(xmin,yhi-ypass, line.c_str());

    line = Form("#sigma: %.2f #pm %.2f (%.2f%s)", sigma.getVal()*1000., sigma.getError()*1000., sigma.getVal()*100./mean.getVal(), "%" );
    lat.DrawLatex(xmin,yhi-2.*ypass, line.c_str());

    //sprintf(line,"S/B(3#sigma): %.2f #pm %.2f", pi0res.SoB, pi0res.SoBerr );
    line = Form("S/B(3#sigma): %.2f", pi0res.SoB );
    lat.DrawLatex(xmin,yhi-3.*ypass, line.c_str());

    line = Form("#Chi^{2}: %.2f (%d dof)", pi0res.chi2, pi0res.dof );
    lat.DrawLatex(xmin,yhi-4.*ypass, line.c_str());

    line = Form("B param. %d", cbpars.getSize() );
    lat.DrawLatex(xmin,yhi-5.*ypass, line.c_str());

    canvas->RedrawAxis("sameaxis");

    Pi0FitResult fitres = pi0res;
    //xframe->chiSquare() is the chi2 reduced, i.e., that whose expected value is 1
    // E[X^2]=v; Var[X^2]=2v --> fit is bad if |X^2-v|>5*sqrt(2v) 

    //if(mode==Pi0EB && ( xframe->chiSquare()/pi0res.dof>0.35 || pi0res.SoB<0.6 || fabs(mean.getVal()-(Are_pi0_? 0.150:0.62))<0.0000001 ) ){
    //bool badChi2 = fabs(xframe->chiSquare() - pi0res.dof) > 5.0 * sqrt(2. * pi0res.dof);

    if(mode==Pi0EB && ( fabs(mean.getVal()-(Are_pi0_? upper_bound_pi0mass_EB:upper_bound_etamass_EB))<0.0000001 ) ){
	  if(niter==0) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 1, isNot_2010_);
	  if(niter==1) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 2, isNot_2010_);
	  if(niter==2) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 3, isNot_2010_);
    }
    if(mode==Pi0EE && ( fabs(mean.getVal()-(Are_pi0_? upper_bound_pi0mass_EE:upper_bound_etamass_EE))<0.0000001 ) ){
	  if(niter==0) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 1, isNot_2010_);
	  if(niter==1) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 2, isNot_2010_);
	  if(niter==2) fitres = FitMassPeakRooFit( h, xlo, xhi, HistoIndex, ngaus, mode, 3, isNot_2010_);
    }

    // save last version of fit made
    // if(StoreForTest_ && niter==0){
    if(StoreForTest_){
      outfileTEST_->cd();
      xframe->Write();
      canvas->Write();
    }

    delete canvas;
    return fitres;
}


//------------------------------------------------
// method to fit E/Etrue

//-----------------------------------------------------------------------------------

//=====================================================================

Double_t my2sideCrystalBall(double* x, double* par) {

  // implementation of a 2-sided crystal ball
  //a priori we allow for different shape of right and left tail, thus two values of alpha and n 

  Double_t xcur = x[0];
  Double_t alphaL = par[0];
  Double_t nL = par[1];
  Double_t mu = par[2];
  Double_t sigma = par[3];
  Double_t N = par[4];
  Double_t alphaR = par[5];
  Double_t nR = par[6];
  Double_t t = (xcur-mu)/sigma;
  Double_t absAlphaL = fabs((Double_t)alphaL);
  Double_t invAbsAlphaL = 1./absAlphaL;
  Double_t absAlphaR = fabs((Double_t)alphaR);
  Double_t invAbsAlphaR = 1./absAlphaR;

  if ( t<-absAlphaL ) {
    Double_t AL = TMath::Power(nL*invAbsAlphaL,nL)*exp(-0.5*absAlphaL*absAlphaL);
    Double_t BL = nL*invAbsAlphaL - absAlphaL;
    return N*AL*TMath::Power(BL-t,-nL);
  } else if ( t <= absAlphaR )  {
    return N*exp(-0.5*t*t);
  } else {
    Double_t AR = TMath::Power(nR*invAbsAlphaR,nR)*exp(-0.5*absAlphaR*absAlphaR);
    Double_t BR = nR*invAbsAlphaR - absAlphaR;
    return N*AR*TMath::Power(BR+t,-nR);
  }

}


//======================================================

TFitResultPtr FitEpsilonPlot::FitEoverEtruePeak(TH1F* h1, uint32_t HistoIndex, FitMode mode, Bool_t noDrawStatBox) 
{


  //-----------------------------------------------------------------------------------
  // We use the TH1::Fit function here [0] for simplicity
  // [0] https://root.cern.ch/doc/master/classTH1.html#a7e7d34c91d5ebab4fc9bba3ca47dabdd

  int niter = 0; // attempt of the fit, only 1 for the moment
  TString nameHistofit = Form("Fit_n_%u_attempt%d",HistoIndex,niter);

  // add canvas to save rooplot on top (will save this in the file)
  TCanvas* canvas = new TCanvas((nameHistofit+Form("_c")).Data(),"",700,700);
  canvas->cd();
  canvas->SetTickx(1);
  canvas->SetTicky(1);
  canvas->cd();
  canvas->SetRightMargin(0.06);

  h1->Draw("HE");

  Double_t histNorm = h1->GetBinContent(h1->GetMaximumBin()); // cout << "histNorm = " << histNorm << endl;
  //Double_t histMean = h1->GetMean();   //  cout << "histMean = " << histMean << endl;
  Double_t histMean = h1->GetBinCenter(h1->GetMaximumBin());;   //  cout << "histMean = " << histMean << endl;
  // tails are huge, the Std deviation is not a good estimator of the gaussian core's width, use a constant term (but in general it will depend on the crystal)
  //Double_t histStdDev = h1->GetStdDev(); //  cout << "histStdDev = " << histStdDev << endl;
  Double_t histStdDev = 0.1; //  cout << "histStdDev = " << histStdDev << endl;   

  // do a preliminary gaussian fit, but do not draw the function (option 0)
  TF1 *gaussian = new TF1("gaussian","gaus",histMean-1.4*histStdDev, histMean+1.4*histStdDev);
  TFitResultPtr frp1 = h1->Fit(gaussian,"E WL I S Q B R 0","HE", histMean - 1.0 * histStdDev, histMean + 1.0 * histStdDev);
  // TF1 *fitFunction = h1->GetFunction("gaussian");
  // if (fitFunction) {
  //   fitFunction->SetLineColor(kRed);
  //   fitFunction->SetLineWidth(2);
  //   fitFunction->Draw("SAME");
  // }
  int fitStatus = frp1;
  // if gaussian fit was successful, update the gaussian mean and width values that will be used for the crystal ball below
  if (fitStatus == 0) {
    histMean = frp1->Parameter(1);  // par [2] is the gaussian sigma in ROOT
    histStdDev = frp1->Parameter(2);  // par [2] is the gaussian sigma in ROOT
  } else {
    std::cout << "FIT_EPSILON: error occurred in FitEoverEtruePeak when fitting with gaussian. Fit status is " << fitStatus << std::endl;
  }

  // define the fitting function
  TF1*cb1 = new TF1("cb1",&my2sideCrystalBall,histMean-3*histStdDev, histMean+3*histStdDev,7);
  cb1->SetParNames("alphaL","nL","Mean(fit)","Sigma","Const","alphaR","nR");
  cb1->SetParLimits(cb1->GetParNumber("nL"),0.1,15);
  cb1->SetParLimits(cb1->GetParNumber("Mean(CB)"), histMean - histStdDev, histMean + histStdDev);
  cb1->SetParLimits(cb1->GetParNumber("Sigma(CB)"),0.1 * histStdDev, 1.1 * histStdDev);
  cb1->SetParLimits(cb1->GetParNumber("nR"),0.1,15);
  cb1->SetParLimits(cb1->GetParNumber("alphaL"),-5.0,-0.1);
  cb1->SetParLimits(cb1->GetParNumber("alphaR"),0.1,5.0);
  cb1->SetParLimits(cb1->GetParNumber("Const"),0.8*histNorm,1.2*histNorm);
  cb1->SetParameters(-1.4,5,histMean,histStdDev,histNorm,1.4,5);
  cb1->SetLineColor(kGreen+2);
  cb1->SetLineWidth(2);

  TFitResultPtr frp2 = h1->Fit(cb1,"E WL I S Q B R","HE SAMES", histMean - 2.0 * histStdDev, histMean + 2.0 * histStdDev);
  //cout << "checkpoint" << endl; return 0;
  fitStatus = frp2;
  if (fitStatus != 0) {
    std::cout << "FIT_EPSILON: error occurred in FitEoverEtruePeak when fitting with crystal ball. Fit status is " << fitStatus << std::endl;
  }
  if (frp2->Parameter(cb1->GetParNumber("Sigma(CB)")) < 0.0 ) {
    cout << "WARNING: CB sigma is negative!" << endl;
  }

  // get gaussian core of the CB and plot it on top to show how the CB differs from a simple gaussian
  TF1 *gaussCore = new TF1(*(h1->GetFunction("cb1")));
  if (gaussCore) {
    Double_t gaussMean = frp2->Parameter(cb1->GetParNumber("Mean(CB)"));
    Double_t gaussSigma = frp2->Parameter(cb1->GetParNumber("Sigma(CB)"));
    Double_t alphaL = frp2->Parameter(cb1->GetParNumber("alphaL"));
    Double_t alphaR = frp2->Parameter(cb1->GetParNumber("alphaR"));
    gaussCore->DrawF1(gaussMean + fabs(gaussSigma) * -fabs(alphaL), gaussMean + fabs(gaussSigma) * fabs(alphaR),"SAME"); // alphaL < 0, alphaR > 0
    gaussCore->SetLineColor(kRed);
    gaussCore->SetLineWidth(2);
  }

  canvas->Update();
  TPaveStats *statBox = (TPaveStats*)(h1->FindObject("stats"));
  if (statBox) {
    statBox->SetX1NDC(0.65);
    statBox->SetX2NDC(0.92);
    statBox->SetY1NDC(0.59);
    statBox->SetY2NDC(0.91);
    statBox->SetFillColor(0);
    statBox->SetFillStyle(0);
    statBox->SetBorderSize(0);
    statBox->Draw();
  }
  canvas->Update();

  string legHeader = "Fit functions";
  TLegend leg (0.15,0.7,0.6,0.9);
  leg.SetHeader(legHeader.c_str());
  leg.SetFillColor(0);
  leg.SetFillStyle(0);
  leg.SetBorderSize(0);
  leg.AddEntry(h1,"data","L");
  //leg.AddEntry(gaussian,"gauss","l");
  if (gaussCore) leg.AddEntry(gaussCore,"gauss core","l");
  leg.AddEntry(cb1,"double CB","l");
  leg.Draw("same");
  canvas->RedrawAxis("sameaxis");
  
  if (noDrawStatBox) {
    h1->SetStats(0);
    //cout << "No Statistics box" << endl;
  } else {
    //canvas->Update();
    gPad->Update();
    gStyle->SetOptStat(1110);
    gStyle->SetOptFit(1102);
  }

  cout << "FIT_EPSILON: "
       << " mean(CB): " << frp2->Parameter(cb1->GetParNumber("Mean(CB)")) << " +/- " << frp2->ParError(cb1->GetParNumber("Mean(CB)"))
       << " sigma(CB): " << frp2->Parameter(cb1->GetParNumber("Sigma(CB)")) << " +/- " << frp2->ParError(cb1->GetParNumber("Sigma(CB)"))
       << " chi2: " << frp2->Chi2()
       << " DOF: " << frp2->Ndf()
       << " N(fit.param.): " << frp2->NFreeParameters()
       << " prob(chi2): " << frp2->Prob()
       << endl;


  // some parameters do not make sense for the E/Etrue study, but for simplicity we keep the same structure as the mass fit
  // basically we just need the peak position ("Mean(CB)" parameter)

  if(mode==Pi0EB){
    // EBmap_Signal[HistoIndex]=-1;
    // EBmap_Backgr[HistoIndex]=-1;
    EBmap_Chisqu[HistoIndex]=frp2->Chi2();
    EBmap_ndof[HistoIndex]=frp2->Ndf();
    EBmap_mean[HistoIndex]=frp2->Parameter(cb1->GetParNumber("Mean(CB)"));
    EBmap_mean_err[HistoIndex]=frp2->ParError(cb1->GetParNumber("Mean(CB)"));
    EBmap_sigma[HistoIndex]=frp2->Parameter(cb1->GetParNumber("Sigma(CB)"));;
    // EBmap_Snorm[HistoIndex]=-1;
    // EBmap_b0[HistoIndex]=-1;
    // EBmap_b1[HistoIndex]=-1;
    // EBmap_b2[HistoIndex]=-1;
    // EBmap_b3[HistoIndex]=-1;
    // EBmap_Bnorm[HistoIndex]=-1;
  }
  if(mode==Pi0EE){
    // EEmap_Signal[HistoIndex]=-1;
    // EEmap_Backgr[HistoIndex]=-1;
    EEmap_Chisqu[HistoIndex]=frp2->Chi2();
    EEmap_ndof[HistoIndex]=frp2->Ndf();
    EEmap_mean[HistoIndex]=frp2->Parameter(cb1->GetParNumber("Mean(CB)"));
    EEmap_mean_err[HistoIndex]=frp2->ParError(cb1->GetParNumber("Mean(CB)"));
    EEmap_sigma[HistoIndex]=frp2->Parameter(cb1->GetParNumber("Sigma(CB)"));;
    // EEmap_Snorm[HistoIndex]=-1;
    // EEmap_b0[HistoIndex]=-1;
    // EEmap_b1[HistoIndex]=-1;
    // EEmap_b2[HistoIndex]=-1;
    // EEmap_b3[HistoIndex]=-1;
    // EEmap_Bnorm[HistoIndex]=-1;
  }


  // if(StoreForTest_ && niter==0){
  if(StoreForTest_){
    outfileTEST_->cd();
    canvas->Write();
  }

  delete canvas;
  return frp2;

}


// ------------ method called once each job just before starting event loop  ------------
    void 
FitEpsilonPlot::beginJob()
{
    if(StoreForTest_){
      outfileTEST_ = new TFile(fitFileName_.c_str(),"RECREATE");
      if(!outfileTEST_) cout << "WARNING: file " << fitFileName_ << " with fit not created." << endl;
    }
}

// ------------ method called once each job just after ending the event loop  ------------
    void 
FitEpsilonPlot::endJob() 
{
    saveCoefficients();
    if(StoreForTest_){
      cout << "FIT_EPSILON: Fit stored in " << fitFileName_ << endl;
      outfileTEST_->Write();
      outfileTEST_->Close();
    }
}

// ------------ method called when starting to processes a run  ------------
    void 
FitEpsilonPlot::beginRun(edm::Run const&, edm::EventSetup const&)
{
}

// ------------ method called when ending the processing of a run  ------------
    void 
FitEpsilonPlot::endRun(edm::Run const&, edm::EventSetup const&)
{
}

// ------------ method called when starting to processes a luminosity block  ------------
    void 
FitEpsilonPlot::beginLuminosityBlock(edm::LuminosityBlock const&, edm::EventSetup const&)
{
}

// ------------ method called when ending the processing of a luminosity block  ------------
    void 
FitEpsilonPlot::endLuminosityBlock(edm::LuminosityBlock const&, edm::EventSetup const&)
{
}

// ------------ method fills 'descriptions' with the allowed parameters for the module  ------------
void
FitEpsilonPlot::fillDescriptions(edm::ConfigurationDescriptions& descriptions) {
    //The following says we do not know what parameters are allowed so do no validation
    // Please change this to state exactly what you do use, even if it is no parameters
    edm::ParameterSetDescription desc;
    desc.setUnknown();
    descriptions.addDefault(desc);
}

//define this as a plug-in
DEFINE_FWK_MODULE(FitEpsilonPlot);
