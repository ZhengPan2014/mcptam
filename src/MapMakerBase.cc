/*************************************************************************
 *  
 *  
 *  Copyright 2014  Adam Harmat (McGill University) 
 *                      [adam.harmat@mail.mcgill.ca]
 *                  Michael Tribou (University of Waterloo)
 *                      [mjtribou@uwaterloo.ca]
 *
 *  Multi-Camera Parallel Tracking and Mapping (MCPTAM) is free software:
 *  you can redistribute it and/or modify it under the terms of the GNU 
 *  General Public License as published by the Free Software Foundation,
 *  either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 *  MCPTAM is based on the Parallel Tracking and Mapping (PTAM) software.
 *  Copyright 2008 Isis Innovation Limited
 *  
 *  
 ************************************************************************/


//=========================================================================================
//
// Copyright 2012 Adam Harmat, McGill University
// adam.harmat@mail.mcgill.ca
//
// Large parts of this code are from the original PTAM, which are
// Copyright 2008 Isis Innovation Limited
//
//=========================================================================================

#include <mcptam/MapMakerBase.h>
#include <mcptam/Map.h>
#include <mcptam/KeyFrame.h>
#include <mcptam/MapInfo.h>
#include <mcptam/LevelHelpers.h>
#include <mcptam/Utility.h>
#include <mcptam/MapPoint.h>
#include <mcptam/SelectedPointsCov.h>
#include <TooN/SymEigen.h>
#include <visualization_msgs/MarkerArray.h>
#include <ros/common.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <fstream>

MapMakerBase::MapMakerBase(Map& map, TaylorCameraMap &cameras, bool bAdvertise)
  : mMap(map)
  , mmCameraModels(cameras)
  , mNodeHandlePriv("~")
  , mvpSelectedPoints(2, NULL)
  , mnCurrSelected(1)
  , mdSelectedPointsCrossCovNorm(-1)
{
  if(bAdvertise)
  {
    mMapInfoPub = mNodeHandlePriv.advertise<mcptam::MapInfo>("map_info", 1, true);
    mMapPointsPub = mNodeHandlePriv.advertise<pcl::PointCloud<pcl::PointXYZRGB> >("map_points", 1,true);
    mMapPointsCovPub = mNodeHandlePriv.advertise<visualization_msgs::MarkerArray>("map_points_cov", 1,true);
    mMapMKFsPub = mNodeHandlePriv.advertise<visualization_msgs::MarkerArray>("map_mkfs_array", 1,true);
  }
  
  mSelectedPointSub = mNodeHandle.subscribe<geometry_msgs::PointStamped>("clicked_point",10, &MapMakerBase::SelectedPointCallback,this);
  mSelectedPointsCovPub = mNodeHandlePriv.advertise<mcptam::SelectedPointsCov>("selected_points_cov", 1,true);
  
  Reset();
}

void MapMakerBase::Reset()
{
  ROS_DEBUG("MapMakerBase: Reset");
  
  for(unsigned i=0; i < mvpSelectedPoints.size(); ++i)
  {
    if(mvpSelectedPoints[i] == NULL)
      continue;
      
    mvpSelectedPoints[i]->mnUsing--;
  }
  
  mMap.Reset();
  
  mbResetDone = true;
  mbResetRequested = false;
  mState = MM_INITIALIZING;
  mdMaxCov = -1;
}

void MapMakerBase::RequestReset()
{
  ROS_DEBUG("MapMakerBase: RequestReset");
  mbResetDone = false;
  mbResetRequested = true;
  
  // Testing this
  //mMap.mbGood = false;
}

// Finds the closest KeyFrame in a given region
KeyFrame* MapMakerBase::ClosestKeyFrame(KeyFrame &kf, KeyFrameRegion region, bool bSameCamName)
{
  if(region == KF_ONLY_SELF)  // If we're only looking in our MKF neighborhood
    ROS_ASSERT(!bSameCamName);  // Make sure we are not just looking for same cam name otherwise we'll find nothing
  
  double dClosestDist = std::numeric_limits<double>::max();
  KeyFrame *pClosestKF = NULL;
  MultiKeyFrame& parent = *kf.mpParent;
    
  if(region == KF_ONLY_SELF) // Only search through parent's keyframes
  {
    for(KeyFramePtrMap::iterator it = parent.mmpKeyFrames.begin(); it != parent.mmpKeyFrames.end(); it++)
    {
      KeyFrame& currentKF = *(it->second);
      if(&currentKF == &kf)
        continue;
        
      double dDist = kf.Distance(currentKF);
      if(dDist < dClosestDist)
      {
        dClosestDist = dDist;
        pClosestKF = &currentKF;
      }
    }
  }
  else  // Otherwise search all keyframes in the map
  {
    for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
    {
      MultiKeyFrame& mkf = *(*it);
      
      if(&mkf == &parent && region == KF_ONLY_OTHER)
        continue;
      
      for(KeyFramePtrMap::iterator jit = mkf.mmpKeyFrames.begin(); jit != mkf.mmpKeyFrames.end(); ++jit)
      {
        KeyFrame& currentKF = *(jit->second);
        if(&currentKF == &kf)
          continue;
          
        if(bSameCamName && currentKF.mCamName != kf.mCamName)
          continue;
          
        double dDist = kf.Distance(currentKF);
        if(dDist < dClosestDist)
        {
          dClosestDist = dDist;
          pClosestKF = &currentKF;
        }
      }
    }
  }
  
  if( !bSameCamName && pClosestKF == NULL )
  {
    // Dump the cameras and map data
    DumpToFile("fail_map.dat");
    ROS_ASSERT(pClosestKF != NULL);
  }
  
  return pClosestKF;
}

// Finds nMaxNum closest KeyFrames, within a given distance, within a given region 
std::vector<KeyFrame*>  MapMakerBase::ClosestKeyFramesWithinDist(KeyFrame &kf, double dThreshDist, unsigned nMaxNum, KeyFrameRegion region)
{
  std::vector<KeyFrame*> vResult;
  
  std::vector<std::pair<double, KeyFrame*> > vpDistsAndKeyFrames;
  MultiKeyFrame& parent = *kf.mpParent;
    
  if(region == KF_ONLY_SELF) // Only search through parent's keyframes
  {
    for(KeyFramePtrMap::iterator it = parent.mmpKeyFrames.begin(); it != parent.mmpKeyFrames.end(); it++)
    {
      KeyFrame& currentKF = *(it->second);
      if(&currentKF == &kf)
        continue;
        
      double dDist = kf.Distance(currentKF);
      if(dDist <= dThreshDist)
      {
        vpDistsAndKeyFrames.push_back(std::make_pair(dDist, &currentKF));
      }
    }
  }
  else  // Otherwise search all keyframes in the map
  {
    for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
    {
      MultiKeyFrame& mkf = *(*it);
      
      if(&mkf == &parent && region == KF_ONLY_OTHER)
        continue;
      
      for(KeyFramePtrMap::iterator jit = mkf.mmpKeyFrames.begin(); jit != mkf.mmpKeyFrames.end(); ++jit)
      {
        KeyFrame& currentKF = *(jit->second);
        if(&currentKF == &kf)
          continue;
          
        double dDist = kf.Distance(currentKF);
        if(dDist <= dThreshDist)
        {
          vpDistsAndKeyFrames.push_back(std::make_pair(dDist, &currentKF));
        }
      }
    }
  }
  
  if(!vpDistsAndKeyFrames.empty())
  {
    if(nMaxNum > vpDistsAndKeyFrames.size()) // if we expect too many neighbors
      nMaxNum = vpDistsAndKeyFrames.size();  // reduce number that will be returned
      
    // Sort the first nMaxNum entries by score
    std::partial_sort(vpDistsAndKeyFrames.begin(), vpDistsAndKeyFrames.begin() + nMaxNum, vpDistsAndKeyFrames.end());
    
    for(unsigned int i=0; i<nMaxNum; i++)
      vResult.push_back(vpDistsAndKeyFrames[i].second);
  }
    
  return vResult;
}

// Finds N closest KeyFrames
std::vector<KeyFrame*> MapMakerBase::NClosestKeyFrames(KeyFrame &kfSrc, unsigned int N)
{
  std::vector<std::pair<double, KeyFrame* > > vScoresAndKFs;
  for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
  {
    MultiKeyFrame& mkf = *(*it);
    
    // Have to iterate through each MultiKeyFrame as well to get KeyFrames
    for(KeyFramePtrMap::iterator jit = mkf.mmpKeyFrames.begin(); jit != mkf.mmpKeyFrames.end(); jit++)
    {
      KeyFrame& kf = *(jit->second);
      
      if(&kf == &kfSrc)
        continue;
      
      double dDist = kfSrc.Distance(kf);
      vScoresAndKFs.push_back(std::make_pair(dDist, &kf));
    }
  }
    
  if(N > vScoresAndKFs.size()) // if we expect too many neighbors
    N = vScoresAndKFs.size();  // reduce numbe that will be returned
    
  // Sort the first N entries by score
  std::partial_sort(vScoresAndKFs.begin(), vScoresAndKFs.begin() + N, vScoresAndKFs.end());
  
  std::vector<KeyFrame*> vResult;
  for(unsigned int i=0; i<N; i++)
    vResult.push_back(vScoresAndKFs[i].second);
    
  return vResult;
}

// Finds the closest MultiKeyFrame
MultiKeyFrame* MapMakerBase::ClosestMultiKeyFrame(MultiKeyFrame &mkf)
{
  double dClosestDist = std::numeric_limits<double>::max();
  MultiKeyFrame *pClosestMKF = NULL;
  
  // used for debugging only
  std::vector<double> vDists;
  
  for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
  {
    MultiKeyFrame& currentMKF = *(*it);
    
    if(&currentMKF == &mkf)
      continue;
        
    double dDist = mkf.Distance(currentMKF);
    vDists.push_back(dDist);
   
    if(dDist < dClosestDist)
    {
      dClosestDist = dDist;
      pClosestMKF = &currentMKF;
    }
  }
  
  if(!pClosestMKF)
  {
    ROS_FATAL_STREAM("Couldn't find closest MKF!");
    ROS_FATAL_STREAM("Got "<<vDists.size()<<" distances:");
    for(unsigned i=0; i < vDists.size(); ++i)
      ROS_FATAL_STREAM(vDists[i]);
      
    ROS_BREAK();
  }

  return pClosestMKF;
}

// Finds N closest MultiKeyFrames
std::vector<MultiKeyFrame*> MapMakerBase::NClosestMultiKeyFrames(MultiKeyFrame &mkfSrc, unsigned int N)
{
  std::vector<std::pair<double, MultiKeyFrame* > > vScoresAndMKFs;
  for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
  {
    MultiKeyFrame& mkf = *(*it);
    
    if(&mkf == &mkfSrc)
      continue;
    
    double dDist = mkfSrc.Distance(mkf);
    vScoresAndMKFs.push_back(std::make_pair(dDist, &mkf));
  }
    
  if(N > vScoresAndMKFs.size()) // if we expect too many neighbors
    N = vScoresAndMKFs.size();  // reduce numbe that will be returned
    
  // Sort the first N entries by score
  std::partial_sort(vScoresAndMKFs.begin(), vScoresAndMKFs.begin() + N, vScoresAndMKFs.end());
  
  std::vector<MultiKeyFrame*> vResult;
  for(unsigned int i=0; i<N; i++)
    vResult.push_back(vScoresAndMKFs[i].second);
    
  return vResult;
}

// Finds the futhest MultiKeyFrame
MultiKeyFrame* MapMakerBase::FurthestMultiKeyFrame(MultiKeyFrame &mkf)
{
  double dFurthestDist = 0;
  MultiKeyFrame *pFurthestMKF = NULL;
  
  for(MultiKeyFramePtrList::iterator it = mMap.mlpMultiKeyFrames.begin(); it != mMap.mlpMultiKeyFrames.end(); ++it)
  {
    MultiKeyFrame& currentMKF = *(*it);
    
    if(&currentMKF == &mkf)
      continue;
        
    double dDist = mkf.Distance(currentMKF);
    if(dDist > dFurthestDist)
    {
      dFurthestDist = dDist;
      pFurthestMKF = &currentMKF;
    }
  }

  ROS_ASSERT(pFurthestMKF != NULL);
  return pFurthestMKF;
}

// Publish information about the map
void MapMakerBase::PublishMapInfo()
{
  if(!mMapInfoPub)
    mMapInfoPub = mNodeHandlePriv.advertise<mcptam::MapInfo>("map_info", 1);
    
  boost::mutex::scoped_lock lock(mMap.mMutex);
  mcptam::MapInfo info_msg;
  info_msg.header.stamp = ros::Time::now();
  
  info_msg.nNumPoints = mMap.mlpPoints.size();
  info_msg.nNumPointsInTrash = mMap.mlpPointsTrash.size();
  info_msg.nNumMultiKeyFrames = mMap.mlpMultiKeyFrames.size();
  
  mMapInfoPub.publish(info_msg);
}

// Publish map point PCL cloud
void MapMakerBase::PublishMapPoints()
{
  if(!mMapPointsPub)
    mMapPointsPub = mNodeHandlePriv.advertise<pcl::PointCloud<pcl::PointXYZRGB> >("map_points", 1,true);
    
  if(!mMapPointsCovPub)
    mMapPointsCovPub = mNodeHandlePriv.advertise<visualization_msgs::MarkerArray>("map_points_cov", 1,true);
    
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointMsg (new pcl::PointCloud<pcl::PointXYZRGB>());
  visualization_msgs::MarkerArray pointCovMsg;
  
  boost::mutex::scoped_lock lock(mMap.mMutex);
  
  ros::Time nowTime = ros::Time::now();
  
  // Fill in header stuff for PCL message
  pointMsg->header.frame_id = "vision_world";
  pointMsg->width = mMap.mlpPoints.size();
  pointMsg->height = 1;
  pointMsg->is_dense = false;
  
#if ROS_VERSION_MINIMUM(1, 9, 54)   // Hydro or above, uses new PCL library
  pointMsg->header.stamp = nowTime.toNSec();
#else
  pointMsg->header.stamp = nowTime;
#endif

  TooN::Matrix<3> m3Zeros = TooN::Zeros;   // for testing covariance matrix
  
  for(MapPointPtrList::iterator point_it = mMap.mlpPoints.begin(); point_it != mMap.mlpPoints.end(); ++point_it)
  {
    MapPoint& point = *(*point_it);
    TooN::Vector<3> v3Color = gavLevelColors[point.mnSourceLevel];
    
    // ------ PCL message ------------
    pcl::PointXYZRGB pclPoint;
    pclPoint.x = point.mv3WorldPos[0];
    pclPoint.y = point.mv3WorldPos[1];
    pclPoint.z = point.mv3WorldPos[2];
    
     // pack r/g/b into rgb
    uint8_t r = roundf(255*v3Color[0]);
    uint8_t g = roundf(255*v3Color[1]);
    uint8_t b = roundf(255*v3Color[2]);
    uint32_t rgb = ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
    pclPoint.rgb = *reinterpret_cast<float*>(&rgb);
     
    pointMsg->points.push_back(pclPoint);
    
    // ------- Marker Array message -----------
    TooN::SO3<> so3Rot = TooN::SO3<>();
    TooN::Vector<3> v3Scale = TooN::makeVector(0.05, 0.05, 0.05);
    bool bUseSphere = false;
    
    if(point.mm3WorldCov != m3Zeros)
    {
      TooN::SymEigen<3> eigCov(point.mm3WorldCov);
      TooN::Matrix<3> m3EVectors = eigCov.get_evectors().T();
      
      if(fabs((m3EVectors[0] ^ m3EVectors[1]) * m3EVectors[2]) > 1e-10)  // eigen vectors are not degenerate config
      {
        bUseSphere = true;
      
        if((m3EVectors[0] ^ m3EVectors[1]) * m3EVectors[2] < 0)  // need to flip direction of first row
        {
          m3EVectors.T()[0] *= -1;
        }
          
        so3Rot = TooN::SO3<>(m3EVectors);
        v3Scale += eigCov.get_evalues();
      }
    }
    
    visualization_msgs::Marker pointMarker;
    
    pointMarker.header.stamp = nowTime;
    pointMarker.header.frame_id = "vision_world";
    
    if(bUseSphere)
      pointMarker.type = visualization_msgs::Marker::SPHERE;
    else
      pointMarker.type = visualization_msgs::Marker::CUBE;
      
    pointMarker.action = visualization_msgs::Marker::ADD;
    pointMarker.ns = "map";
    pointMarker.id = reinterpret_cast<uint64_t>(&point);
    pointMarker.pose.position.x = point.mv3WorldPos[0];
    pointMarker.pose.position.y = point.mv3WorldPos[1];
    pointMarker.pose.position.z = point.mv3WorldPos[2];
    pointMarker.pose.orientation = util::SO3ToQuaternionMsg(so3Rot);
    pointMarker.scale.x = v3Scale[0];
    pointMarker.scale.y = v3Scale[1];
    pointMarker.scale.z = v3Scale[2];
    pointMarker.color.a = 1.0;
    pointMarker.color.r = v3Color[0];
    pointMarker.color.g = v3Color[1];
    pointMarker.color.b = v3Color[2];
    pointMarker.lifetime = ros::Duration(0);
    
    pointCovMsg.markers.push_back(pointMarker);
    
  }  // end loop over map points
  
  mMapPointsPub.publish(pointMsg);
  mMapPointsCovPub.publish(pointCovMsg);
}

// Publish MKFs as a marker array
void MapMakerBase::PublishMapMKFs()
{
  if(!mMapMKFsPub)
    mMapMKFsPub = mNodeHandlePriv.advertise<visualization_msgs::MarkerArray>("map_mkfs_array", 1,true);
  
  visualization_msgs::MarkerArray mkfsMsg;
  
  boost::mutex::scoped_lock lock(mMap.mMutex);
  
  for(MultiKeyFramePtrList::iterator mkf_it = mMap.mlpMultiKeyFrames.begin(); mkf_it != mMap.mlpMultiKeyFrames.end(); ++mkf_it)
  {
    MultiKeyFrame& mkf = *(*mkf_it);
    visualization_msgs::Marker marker;
    MKFToMarker(mkf, marker);
    mkfsMsg.markers.push_back(marker);
  }
  
  mMapMKFsPub.publish(mkfsMsg);
}

// Converts an MKF to a visualization  message
void MapMakerBase::MKFToMarker(MultiKeyFrame& mkf, visualization_msgs::Marker& marker)
{
  marker.header.stamp = ros::Time(0);
  marker.header.frame_id = "vision_world";
  marker.type = visualization_msgs::Marker::LINE_LIST;
  marker.action = visualization_msgs::Marker::ADD;
  marker.ns = "MKF";
  marker.id = reinterpret_cast<uint64_t>(&mkf);
  
  marker.pose = util::SE3ToPoseMsg(mkf.mse3BaseFromWorld.inverse());

  float scale = 0.5;
  marker.scale.x = scale/10;
  marker.color.a = 1.0;
  marker.color.r = 0.0;
  marker.color.g = 0.0;
  marker.color.b = 0.0;
  
  marker.points.resize(6);
  marker.colors.resize(6);
  // point should default to origin, only need to set pts 1 3 and 5
  marker.points[1].x = scale;
  marker.points[3].y = scale;
  marker.points[5].z = scale;
  
  // X axis red, Y axis green, Z axis blue
  marker.colors[0].r = 1;
  marker.colors[0].a = 1;
  marker.colors[1].r = 1;
  marker.colors[1].a = 1;
  marker.colors[2].g = 1;
  marker.colors[2].a = 1;
  marker.colors[3].g = 1;
  marker.colors[3].a = 1;
  marker.colors[4].b = 1;
  marker.colors[4].a = 1;
  marker.colors[5].b = 1;
  marker.colors[5].a = 1;
  
  marker.lifetime = ros::Duration(0);
}

// Publish the map points, then the MKFs
void MapMakerBase::PublishMapVisualization()
{
  ROS_DEBUG(" >>>>>>>>>>>>>>>> PUBLISHING MAP VISUALIZATION <<<<<<<<<<<<<<<<<<<<<<<");
  PublishMapPoints();
  PublishMapMKFs();
  
  PublishSelectedPointsCov();
}

// Dumps all map information to a file
void MapMakerBase::DumpToFile(std::string filename)
{ 
  std::ofstream ofs(filename.c_str());
  
  if(!ofs.good())
  {
    ROS_ERROR_STREAM("Couldn't open "<<filename<<" to write map");
    return;
  }
  
  // First write CamFromBase for each camera in system
  ofs<<"% Camera poses in MKF frame, format:"<<std::endl;
  ofs<<"% Total number of cameras"<<std::endl;
  ofs<<"% Camera Name, Position (3 vector), Orientation (quaternion, 4 vector)"<<std::endl;
  
  MultiKeyFrame* pFirstMKF = *(mMap.mlpMultiKeyFrames.begin());
  
  // Total number of cameras
  ofs<<pFirstMKF->mmpKeyFrames.size()<<std::endl;
  
  for(KeyFramePtrMap::iterator kf_it = pFirstMKF->mmpKeyFrames.begin(); kf_it != pFirstMKF->mmpKeyFrames.end(); ++kf_it)
  {
    KeyFrame& kf = *(kf_it->second);
    
    // Store the conventional way of defining pose (ie inverse of PTAM)
    geometry_msgs::Pose pose = util::SE3ToPoseMsg(kf.mse3CamFromBase.inverse());
    
    ofs<<kf.mCamName;
    ofs<<", "<<pose.position.x<<", "<<pose.position.y<<", "<<pose.position.z;
    ofs<<", "<<pose.orientation.x<<", "<<pose.orientation.y<<", "<<pose.orientation.z<<", "<<pose.orientation.w<<std::endl;
  }
  
  // Now write BaseFromWorld for each MKF in system
  ofs<<"% MKFs in world frame, format:"<<std::endl;
  ofs<<"% Total number of MKFs"<<std::endl;
  ofs<<"% MKF number, Position (3 vector), Orientation (quaternion, 4 vector)"<<std::endl;
  
  // Total number of MKFs
  ofs<<mMap.mlpMultiKeyFrames.size()<<std::endl;
  
  int i=0;
  for(MultiKeyFramePtrList::iterator mkf_it = mMap.mlpMultiKeyFrames.begin(); mkf_it != mMap.mlpMultiKeyFrames.end(); ++i, ++mkf_it)
  {
    MultiKeyFrame& mkf = *(*mkf_it);
    mkf.mnID = i;
    
    // Store the conventional way of defining pose (ie inverse of PTAM)
    geometry_msgs::Pose pose = util::SE3ToPoseMsg(mkf.mse3BaseFromWorld.inverse());
    
    ofs<<i;
    ofs<<", "<<pose.position.x<<", "<<pose.position.y<<", "<<pose.position.z;
    ofs<<", "<<pose.orientation.x<<", "<<pose.orientation.y<<", "<<pose.orientation.z<<", "<<pose.orientation.w<<std::endl;
  }
  
  // Now write map point positions
  ofs<<"% Points in world frame, format:"<<std::endl;
  ofs<<"% Total number of points"<<std::endl;
  ofs<<"% Point number, Position (3 vector), Parent MKF number, Parent camera name"<<std::endl;
  
  // Total number of points
  ofs<<mMap.mlpPoints.size()<<std::endl;
  
  int nTotalMeas = 0;
  i=0;
  for(MapPointPtrList::iterator point_it = mMap.mlpPoints.begin(); point_it != mMap.mlpPoints.end(); ++i, ++point_it)
  {
    MapPoint& point = *(*point_it);
    point.mnID = i;
    
    ofs<<i;
    ofs<<", "<<point.mv3WorldPos[0]<<", "<<point.mv3WorldPos[1]<<", "<<point.mv3WorldPos[2];
    ofs<<", "<<point.mpPatchSourceKF->mpParent->mnID<<", "<<point.mpPatchSourceKF->mCamName<<std::endl;
    
    nTotalMeas += point.mMMData.spMeasurementKFs.size();
  }
  
  // Now write measurements
  ofs<<"% Measurements of points from KeyFrames, format: "<<std::endl;
  ofs<<"% Total number of measurements"<<std::endl;
  ofs<<"% MKF number, camera name, point number, image position (2 vector) at level 0, measurement noise"<<std::endl;
  
  // Total number of measurements
  ofs<<nTotalMeas<<std::endl;
  
  for(MultiKeyFramePtrList::iterator mkf_it = mMap.mlpMultiKeyFrames.begin(); mkf_it != mMap.mlpMultiKeyFrames.end(); ++mkf_it)
  {
    MultiKeyFrame& mkf = *(*mkf_it);
    for(KeyFramePtrMap::iterator kf_it = mkf.mmpKeyFrames.begin(); kf_it != mkf.mmpKeyFrames.end(); ++kf_it)
    {
      KeyFrame& kf = *(kf_it->second);
      for(MeasPtrMap::iterator meas_it = kf.mmpMeasurements.begin(); meas_it != kf.mmpMeasurements.end(); ++meas_it)
      {
        MapPoint& point = *(meas_it->first);
        Measurement& meas = *(meas_it->second);
        
        ofs<<mkf.mnID<<", "<<kf.mCamName<<", "<<point.mnID<<", ";
        ofs<<meas.v2RootPos[0]<<", "<<meas.v2RootPos[1]<<", "<<LevelScale(meas.nLevel) * LevelScale(meas.nLevel)<<std::endl;
      }
    }
  }
  
  // Done writing
  ofs<<"% The end";
  ofs.close();
  
}

void MapMakerBase::SelectedPointCallback(const geometry_msgs::PointStamped::ConstPtr& pointMsg)
{
  // Find closest point to pointMsg
  TooN::Vector<3> v3ClickedWorld = TooN::makeVector(pointMsg->point.x, pointMsg->point.y, pointMsg->point.z);
  std::cout<<"In SelectedPointCallback, world cooords: "<<v3ClickedWorld<<std::endl;
  
  MapPoint* pClosestPoint = NULL;
  double dMinDist = std::numeric_limits<double>::max();
  
  for(MapPointPtrList::iterator point_it = mMap.mlpPoints.begin(); point_it != mMap.mlpPoints.end(); ++point_it)
  {
    MapPoint* pPoint = *point_it;
    double dDist = TooN::norm(pPoint->mv3WorldPos - v3ClickedWorld);
    if(dDist < dMinDist)
    {
      dMinDist = dDist;
      pClosestPoint = pPoint;
    }
  }
  
  std::cout<<"Minimum distance was: "<<dMinDist<<std::endl;
  
  if(pClosestPoint == NULL)
    return;
    
  if(dMinDist > 0.2)
    return;
    
  pClosestPoint->mnUsing++;
    
  // Decrement using count if we already have a point in the target slot
  if(mvpSelectedPoints[1-mnCurrSelected] != NULL)
    mvpSelectedPoints[1-mnCurrSelected]->mnUsing--;
    
  mvpSelectedPoints[1-mnCurrSelected] = pClosestPoint;  
  mnCurrSelected = 1-mnCurrSelected;
}

void MapMakerBase::PublishSelectedPointsCov()
{
  mcptam::SelectedPointsCov covMsg;
  
  if(mvpSelectedPoints[0] != NULL)
    covMsg.cov_norm_1 = TooN::norm_fro(mvpSelectedPoints[0]->mm3WorldCov);
  
  if(mvpSelectedPoints[1] != NULL)
    covMsg.cov_norm_2 = TooN::norm_fro(mvpSelectedPoints[1]->mm3WorldCov);
    
  if(mdSelectedPointsCrossCovNorm > 0)
    covMsg.cov_norm_x = mdSelectedPointsCrossCovNorm;
    
  mSelectedPointsCovPub.publish(covMsg);
}
