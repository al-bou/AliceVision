#include <openMVG/localization/VoctreeLocalizer.hpp>
#include <openMVG/localization/CCTagLocalizer.hpp>
#include <openMVG/rig/Rig.hpp>
#include <openMVG/sfm/pipelines/localization/SfM_Localizer.hpp>
#include <openMVG/image/image_io.hpp>
#include <openMVG/dataio/FeedProvider.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/progress.hpp>
#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>

#include <iostream>
#include <string>
#include <chrono>

#if HAVE_ALEMBIC
#include <openMVG/dataio/AlembicExporter.hpp>
#endif // HAVE_ALEMBIC

#define POPART_COUT(x) std::cout << x << std::endl
#define POPART_CERR(x) std::cerr << x << std::endl


namespace bfs = boost::filesystem;
namespace bacc = boost::accumulators;
namespace po = boost::program_options;

using namespace openMVG;

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  std::stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

int main(int argc, char** argv)
{
  std::string sfmFilePath; //< the OpenMVG .json data file
  std::string descriptorsFolder; //< the OpenMVG .json data file
  std::string mediaFilepath; //< the media file to localize
  localization::CCTagLocalizer::Parameters param = localization::CCTagLocalizer::Parameters();
#if HAVE_ALEMBIC
  std::string exportFile = "trackedcameras.abc"; //!< the export file
#endif
  std::size_t nCam = 3;
  po::options_description desc(
          "This program takes as input a media (image, image sequence, video) and a database (voctree, 3D structure data) \n"
          "and returns for each frame a pose estimation for the camera.");
  desc.add_options()
          ("help,h", "Print this message")
          ("results,r", po::value<size_t>(&param._nNearestKeyFrames)->default_value(param._nNearestKeyFrames), "Number of images to retrieve in database")
          ("sfmdata,d", po::value<std::string>(&sfmFilePath)->required(), "The sfm_data.json kind of file generated by OpenMVG [it could be also a bundle.out to use an older version of OpenMVG]")
          ("siftPath,s", po::value<std::string>(&descriptorsFolder), "Folder containing the .desc. If not provided, it will be assumed to be parent(sfmdata)/matches [for the older version of openMVG it is the list.txt]")
          ("mediafile,m", po::value<std::string>(&mediaFilepath)->required(), "The folder path containing all the synchronised image subfolders assocated to each camera")
          ("refineIntrinsics,", po::bool_switch(&param._refineIntrinsics), "Enable/Disable camera intrinsics refinement for each localized image")
          ("nCameras", po::value<size_t>(&nCam)->default_value(nCam), "Enable/Disable camera intrinsics refinement for each localized image")
#if HAVE_ALEMBIC
          ("export,e", po::value<std::string>(&exportFile)->default_value(exportFile), "Filename for the SfM_Data export file (where camera poses will be stored). Default : trackedcameras.json If Alambic is enable it will also export an .abc file of the scene with the same name")
#endif
          ;

  po::variables_map vm;

  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help") || (argc == 1)) {
      POPART_COUT(desc);
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }  catch (boost::program_options::required_option& e) {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }  catch (boost::program_options::error& e) {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  {
    POPART_COUT("Program called with the following parameters:");
    POPART_COUT("\tsfmdata: " << sfmFilePath);
    POPART_COUT("\tmediafile: " << mediaFilepath);
    POPART_COUT("\tsiftPath: " << descriptorsFolder);
    POPART_COUT("\tresults: " << param._nNearestKeyFrames);
    POPART_COUT("\trefineIntrinsics: " << param._refineIntrinsics);
  }

  // init the localizer
  localization::CCTagLocalizer localizer;
  bool isInit = localizer.init(sfmFilePath, descriptorsFolder);

  if (!isInit) {
    POPART_CERR("ERROR while initializing the localizer!");
    return EXIT_FAILURE;
  }

#if HAVE_ALEMBIC
  dataio::AlembicExporter exporter(exportFile);
  exporter.addPoints(localizer.getSfMData().GetLandmarks());
#endif
  
  // Create a camera rig
  rig::Rig rig;

  // Loop over all cameras of the rig
  for (int iLocalizer = 0; iLocalizer < nCam; ++iLocalizer)
  {
    std::string subMediaFilepath, calibFile;
    subMediaFilepath = mediaFilepath + "/" + std::to_string(iLocalizer);
    calibFile = subMediaFilepath + "/intrinsics.txt";

    // create the feedProvider
    dataio::FeedProvider feed(subMediaFilepath, calibFile);
    if (!feed.isInit())
    {
      POPART_CERR("ERROR while initializing the FeedProvider!");
      return EXIT_FAILURE;
    }

    //std::string featureFile, cameraResultFile, pointsFile;
    //featureFile = subMediaFilepath + "/cctag" + std::to_string(nRings) + "CC.out";
    //cameraResultFile = inputFolder + "/" + std::to_string(i) + "/cameras.txt";
    //std::ofstream result;
    //result.open(cameraResultFile);
    //pointsFile = inputFolder + "/points.txt";

    image::Image<unsigned char> imageGrey;
    cameras::Pinhole_Intrinsic_Radial_K3 queryIntrinsics;
    bool hasIntrinsics = false;
    geometry::Pose3 cameraPose;

    size_t frameCounter = 0;
    std::string currentImgName;

    // Define an accumulator set for computing the mean and the
    // standard deviation of the time taken for localization
    bacc::accumulator_set<double, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::sum > > stats;

    // used to collect the match data result
    std::vector<sfm::Image_Localizer_Match_Data> associations;
    std::vector<geometry::Pose3> poses;
    std::vector<std::vector<pair<IndexT, IndexT> > > associationIDs;
    std::vector<bool> localized; // this is just to keep track of the unlocalized frames so that a fake camera
    // can be inserted and we see the sequence correctly in maya
    
    std::vector<localization::LocalizationResult> vLocalizationResults;
    while (feed.next(imageGrey, queryIntrinsics, currentImgName, hasIntrinsics))
    {
      POPART_COUT("******************************");
      POPART_COUT("FRAME " << myToString(frameCounter, 4));
      POPART_COUT("******************************");
      sfm::Image_Localizer_Match_Data matchData;
      std::vector<pair<IndexT, IndexT> > ids;
      auto detect_start = std::chrono::steady_clock::now();
      localization::LocalizationResult localizationResult;
      localizer.localize(imageGrey,
              param,
              hasIntrinsics/*useInputIntrinsics*/,
              queryIntrinsics,
              localizationResult);
      vLocalizationResults.emplace_back(localizationResult);
      auto detect_end = std::chrono::steady_clock::now();
      auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
      POPART_COUT("\nLocalization took  " << detect_elapsed.count() << " [ms]");
      stats(detect_elapsed.count());

      ++frameCounter;
    }

    rig.setTrackingResult(vLocalizationResults, iLocalizer);
    
    // print out some time stats
    POPART_COUT("\n\n******************************");
    POPART_COUT("Localized " << frameCounter << " images");
    POPART_COUT("Processing took " << bacc::sum(stats) / 1000 << " [s] overall");
    POPART_COUT("Mean time for localization:   " << bacc::mean(stats) << " [ms]");
    POPART_COUT("Max time for localization:   " << bacc::max(stats) << " [ms]");
    POPART_COUT("Min time for localization:   " << bacc::min(stats) << " [ms]");
  }
  POPART_COUT("Rig calibration initialization");
  rig.initializeCalibration();
  POPART_COUT("Rig calibration optimization");
  rig.optimizeCalibration();
}
