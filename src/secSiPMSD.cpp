#include "secSiPMSD.hh"
#include "secScintSD.hh"
#include "secAnalysis.hh"
#include "secParticleSource.hh"
#include "G4SDManager.hh"
#include "G4HCofThisEvent.hh"
#include "G4Threading.hh"
#include "G4Step.hh"
#include "G4Track.hh"
#include "G4OpticalPhoton.hh"
#include "G4PhysicalVolumeStore.hh"
#include "G4SystemOfUnits.hh"
#include "tools/histo/h1d"
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cassert>

class G4VPhysicalVolume;

secSiPMSD::secSiPMSD(const G4String &SDname, const std::vector<G4String> SDHCnameVect, secScintSD* pSD) : 
    G4VSensitiveDetector(SDname),
    DecayEventID(0),
    NoiseResponseID(0),
    NormalResponseID(0),
    IsMuon(false),
    IsNoise(false),
    HasEntered(false),
    EventWaitTime(0.),
    pScintSD(pSD),
    pHCup(nullptr),
    pHCdown(nullptr)
{    
/* register the names of the HCs to G4VSensitiveDetector object so
    that the HCids which are assigned by G4 Kenel could be assessed by
    G4SDManager object */
    for(G4String str : SDHCnameVect)
    {
        collectionName.insert(str);
    }

    std::ifstream ifstrm;
    ifstrm.open("NoiseWaitTime.dat", std::ifstream::in);
    if( ifstrm.is_open() )
    {
        std::string line;
        while( getline(ifstrm, line) )
        {
            NoiseWaitTimeVect.push_back( atof( line.c_str() ) );
        }
    }
    ifstrm.close();
}

secSiPMSD::~secSiPMSD()
{

}

void secSiPMSD::Initialize(G4HCofThisEvent* HC)
{
    //SensitiveDetectorName is a string defined in G4VSensitiveDetector.hh
    //collectionName is a STL vector defined in G4CollectionVector.hh

    //create HCs
    auto SDmgr = G4SDManager::GetSDMpointer();
    pHCup   = new secSiPMHitsCollection(SensitiveDetectorName, collectionName[0]);
    pHCdown = new secSiPMHitsCollection(SensitiveDetectorName, collectionName[1]);

    //Get collection IDs from the G4SDManager object, and register the created HCs to G4HCofThisEvent object.
    G4int HCIDup   = SDmgr->GetCollectionID(collectionName[0]);
    G4int HCIDdown = SDmgr->GetCollectionID(collectionName[1]);

    HC->AddHitsCollection(HCIDup, pHCup);
    HC->AddHitsCollection(HCIDdown, pHCdown);

    auto FileMgr = G4RootAnalysisManager::Instance();
    FileMgr->OpenFile("secNoiseData.root");
}

G4bool secSiPMSD::ProcessHits(G4Step* step, G4TouchableHistory* )
{
//the return value of this function is currently researved and
//it may be used in the future update of Geant4 application.
    auto ParticleNow = step->GetTrack()->GetParticleDefinition();    
    if( *ParticleNow != *G4OpticalPhoton::Definition() )
    {
        //not an optical photon, return, DO NOT generate hit
        return false;
    }
//the case that an optical photon hits one of the SiPM
//the copy number of the UPPER SiPM is 1 and that of the lower one is 2!!
    //generate the time stamp for each event!
    IsMuon = pScintSD->IsMuon();
    IsNoise = pScintSD->IsNoise();
    if( !HasEntered && IsMuon )
    {
        HasEntered = true;
        EventWaitTime = secParticleSource::MuonWaitTime();
    }

    if( !HasEntered && IsNoise )
    {
        HasEntered = true;
        EventWaitTime = secParticleSource::NoiseWaitTime();
    }
    
    const G4int VolumeCpyNb = step->GetPreStepPoint()->GetTouchableHandle()->GetCopyNumber();
    G4double aPhotonEneg = step->GetPreStepPoint()->GetKineticEnergy(); //MeV
    G4double GlobalTime  = step->GetTrack()->GetGlobalTime() /*+ EventWaitTime*/;
    //std::cout << "GlobalTime = " << GlobalTime << std::endl;
    step->GetTrack()->SetTrackStatus(fStopAndKill);
    if( VolumeCpyNb == 1 )
    {
        //upper SiPM!!
	//std::cout << "In Up" << std::endl;
	auto newHitUp = new secSiPMHit();
        (*newHitUp).SetPhotonEneg(aPhotonEneg).SetGlobalTime(GlobalTime);
        pHCup->insert(newHitUp);
    }
    else if( VolumeCpyNb == 2 )
    {
        //lower SiPM!!
        //std::cout << "In Down" << std::endl;
        auto newHitDown = new secSiPMHit();
        (*newHitDown).SetPhotonEneg(aPhotonEneg).SetGlobalTime(GlobalTime);
        pHCdown->insert(newHitDown);
    }
    else
    {
        //do nothing.
    }
    //the photons is absorbed by the SiPM, so the track must be killed
    step->GetTrack()->SetTrackStatus(fStopAndKill);
    return true;
}

void secSiPMSD::EndOfEvent(G4HCofThisEvent*)
{   
    if( pFile == nullptr )
    {
        return;
    }
    const G4double BackTimeWindow = 20000*ns;
    const G4double FrontTimeWindow = 100*ns;
    
    if( !(pHCup->GetSize()) && !(pHCdown->GetSize()) ) // empty HC, the PM haven't been triggered!
    {
        ResetDecayFlag();
        //ignore the event that didn't trigger both of the SiPMs!
	    return;
    }
    else if( IsNoise ) // the PM is Triggered by Noise particle( mainly electrons )
    {
        ++NoiseResponseID;
	    G4String UpName = "UpNoiseResponse ", DownName = "DownNoiseResponse ";
        char Buf[50] = {};
        sprintf(Buf, "%d_t%d", NoiseResponseID, G4Threading::G4GetThreadId());
        UpName += Buf;
        DownName += Buf;
        
        //creating Up histograms
        auto FileMgr = G4RootAnalysisManager::Instance();
        FileMgr->CreateH1(UpName, UpName, 800, 0., 2000.*ns);
        G4int UpID = FileMgr->GetH1Id(UpName);

        for(size_t i = 0; i != pHCup->GetSize(); ++i)
        {
            G4double val = ( (*pHCup)[i] )->GetGlobalTime();
            FileMgr->FillH1(UpId, val);
        }
        
        //creating Down Histograms
        FileMgr->CreateH1(DownName, DownName, 800, 0., 2000.*ns);
        G4int DownID = FileMgr->GetH1Id(DownName);

        for(size_t i = 0; i != pHCdown->GetSize(); ++i)
        {
            G4double val = ( (*pHCdown)[i] )->GetGlobalTime();
            FileMgr->FillH1(DownId, val);
        }

        PrintData("NoiseWaitTime.dat", EventWaitTime);
    }
    else if( IsADecayEvent() ) // a decay event
    {
        if( !(pHCup->GetSize()) || !(pHCdown->GetSize()) )
        {
            ResetDecayFlag();
            return;
        }
        DecayEventID++;
        ResetDecayFlag();
        G4String UpName = "UpDecayID ", DownName = "DownDecayID ";
        char Buf[50] = {};
        sprintf(Buf, "%d_t%d", DecayEventID, G4Threading::G4GetThreadId());
        UpName += Buf;
        DownName += Buf;
        
        PrintData("DecayMuonWaitTime.dat", EventWaitTime);
    }
    else // normal muon events
    {
        G4bool IsPrint = false;

        auto beg = NoiseWaitTimeVect.begin();
        auto end = NoiseWaitTimeVect.end();
        auto mid = beg + (end - beg) / 2;
        
        //locate the closest noise wait time
        if( EventWaitTime <= *beg )
        {
            if( *beg - EventWaitTime <= BackTimeWindow )
            {
                //print the response!
                IsPrint = true;
            }
            //else do nothing
        }
        else if ( EventWaitTime >= *(end - 1) )
        {
            if( EventWaitTime - *end <= FrontTimeWindow )
            {
                //print the response
                IsPrint = true;
            }
            //else do nothing
        }
        else
        {
            while( mid != end && EventWaitTime != *mid )
            {
                if( EventWaitTime < *mid )
                    end = mid;
                else
                    beg = mid + 1;
                mid = beg + (end - beg) / 2; // update middle
            }
        }

        if( *mid - EventWaitTime <= BackTimeWindow ||  EventWaitTime - (*mid-1) <= FrontTimeWindow )
        {
            //print the response
            IsPrint = true;
        }
        //else do nothing

        if( IsPrint )
        {
            ++NormalResponseID;
            G4String UpName = "UpNormalID ", DownName = "DownNormalID ";
            char Buf[50] = {};
            sprintf(Buf, "%d", NormalResponseID);
            UpName += Buf;
            DownName += Buf;

    	    PrintData("NormalMuonWaitTime.dat", EventWaitTime);
        }
    }
    //reset the flag for generating the time stamp at the end of the event!
    HasEntered = false;
}

G4bool secSiPMSD::IsADecayEvent()
{
    return pScintSD->DecayFlagSiPM;    
}
void secSiPMSD::ResetDecayFlag()
{
    pScintSD->DecayFlagSiPM = false;
}

//print histogram version
void secSiPMSD::PrintData(G4String FileName, G4String HistName, 
		          secSiPMHitsCollection* pHC, 
			  secSiPMHit::DataGetter Getter, 
			  unsigned int nbins, G4double Xmin, G4double Xmax)
{
    //fill histogram
    tools::histo::h1d hist("aHist", nbins, Xmin, Xmax);
    for( size_t i = 0; i != pHC->GetSize(); ++i )
    {   
        G4double val = ( ( (*pHC)[i] ) ->* (Getter) )();
        hist.fill(val);
    }

    //create files
    std::ostringstream sstrm;
    sstrm << FileName << "_t" << G4Threading::G4GetThreadId();
    std::ofstream fstrm(sstrm.str(), std::ofstream::app | std::ofstream::binary);
    
    if( !fstrm.is_open() )
    {
        std::cerr << "***Unable to open file: " << FileName << " ***" << '\n';
        std::cerr << "***The generated data will not be recorded!***" << '\n';
    }

    //assert( fstrm.is_open() );
    fstrm << HistName + "_t" << G4Threading::G4GetThreadId() << '\n';
    
    //write file
    const std::vector<unsigned int>& entries = hist.bins_entries();
    std::vector<G4double> edges;
    edges.push_back(-1.);
    const G4double binw = (Xmax - Xmin) / nbins;
    for( size_t i = 0; i != nbins + 1; ++i )
    {
	const G4double edge = Xmin + i * binw;
        edges.push_back( edge );
    }
    //std::cout << "entries size = " << entries.size() << " edges size = " << edges.size() << '\n';
    size_t sz = entries.size() <= edges.size() ? entries.size() : edges.size();
    
    for( size_t i = 0; i != sz; ++i )
    {
	
        if( entries[i] == 0 )
	{
            continue;
	}

        fstrm << edges[i] << '\t' << entries[i] << '\n';
    }

    fstrm.flush();
    //close files
    fstrm.close();
}

//direct print HC version
void secSiPMSD::PrintData(G4String FileName, G4String HCname,
                          secSiPMHitsCollection* pHC, 
			  secSiPMHit::DataGetter Getter)
{
    //create files
    std::ostringstream sstrm;
    sstrm << FileName << "_t" << G4Threading::G4GetThreadId();
    std::ofstream fstrm(sstrm.str(), std::ofstream::app | std::ofstream::binary);
    
    if( !fstrm.is_open() )
    {
        std::cerr << "***Unable to open file: " << FileName << " ***" << '\n';
        std::cerr << "***The generated data will not be recorded!***" << '\n';
    }

    //assert( fstrm.is_open() );
    fstrm << HCname + "_t" << G4Threading::G4GetThreadId() << '\n';

    for( size_t i = 0; i != pHC->GetSize(); ++i )
    {
        G4double val = ( ( (*pHC)[i] ) ->* (Getter) )();
        fstrm << val << '\n';
    }    

    fstrm.flush();
    //close files
    fstrm.close();

}

//print single double value version
void secSiPMSD::PrintData(G4String FileName, G4double val)
{
    	std::ostringstream sstrm;
        sstrm << FileName << "_t" << G4Threading::G4GetThreadId();

        std::ofstream fstrm(sstrm.str(), std::ofstream::app | std::ofstream::binary );
        fstrm << val << '\n';
}
