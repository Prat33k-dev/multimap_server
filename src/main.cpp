/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Author: Brian Gerkey */

#define USAGE "\nUSAGE: multimap_server <multimap_server_config.yaml>\n" \
              "  multimap_server_config.yaml: Indicates which maps are going to be loaded and info on how to do it"

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <fstream>

#include "ros/ros.h"
#include "ros/console.h"
#include "multimap_server/image_loader.h"
#include "nav_msgs/MapMetaData.h"
#include "yaml-cpp/yaml.h"
#include <resource_retriever/retriever.h>

#ifdef HAVE_YAMLCPP_GT_0_5_0
// The >> operator disappeared in yaml-cpp 0.5, so this function is
// added to provide support for code written under the yaml-cpp 0.3 API.
template<typename T>
void operator >> (const YAML::Node& node, T& i)
{
  i = node.as<T>();
}
#endif

class Map
{
  public:

    Map(const std::string& fname, const std::string& ns, const std::string& desired_name, const std::string& global_frame_id)
    {
      std::string mapfname = "";
      double origin[3];
      int negate;
      double occ_th, free_th;
      MapMode mode = TRINARY;
      double resolution;

      std::ifstream fin(fname.c_str());
      if (fin.fail()) {
        ROS_ERROR("Multimap_server could not open %s.", fname.c_str());
        exit(-1);
      }
  #ifdef HAVE_YAMLCPP_GT_0_5_0
      // The document loading process changed in yaml-cpp 0.5.
      YAML::Node doc = YAML::Load(fin);
  #else
      YAML::Parser parser(fin);
      YAML::Node doc;
      parser.GetNextDocument(doc);
  #endif

      try {
        doc["resolution"] >> resolution;
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain a resolution tag or it is invalid.");
        exit(-1);
      }
      try {
        doc["negate"] >> negate;
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain a negate tag or it is invalid.");
        exit(-1);
      }
      try {
        doc["occupied_thresh"] >> occ_th;
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain an occupied_thresh tag or it is invalid.");
        exit(-1);
      }
      try {
        doc["free_thresh"] >> free_th;
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain a free_thresh tag or it is invalid.");
        exit(-1);
      }
      try {
        std::string modeS = "";
        doc["mode"] >> modeS;

        if(modeS=="trinary")
          mode = TRINARY;
        else if(modeS=="scale")
          mode = SCALE;
        else if(modeS=="raw")
          mode = RAW;
        else{
          ROS_ERROR("Invalid mode tag \"%s\".", modeS.c_str());
          exit(-1);
        }
      } catch (YAML::Exception) {
        ROS_DEBUG("The map does not contain a mode tag or it is invalid... assuming Trinary");
        mode = TRINARY;
      }
      try {
        doc["origin"][0] >> origin[0];
        doc["origin"][1] >> origin[1];
        doc["origin"][2] >> origin[2];
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain an origin tag or it is invalid.");
        exit(-1);
      }
      try {
        doc["image"] >> mapfname;
        // TODO: make this path-handling more robust
        if(mapfname.size() == 0)
        {
          ROS_ERROR("The image tag cannot be an empty string.");
          exit(-1);
        }
        if(mapfname[0] != '/')
        {
          // dirname can modify what you pass it
          char* fname_copy = strdup(fname.c_str());
          mapfname = std::string(dirname(fname_copy)) + '/' + mapfname;
          free(fname_copy);
        }
      } catch (YAML::InvalidScalar) {
        ROS_ERROR("The map does not contain an image tag or it is invalid.");
        exit(-1);
      }

      ROS_INFO("Loading map from image \"%s\"", mapfname.c_str());
      try
      {
          multimap_server::loadMapFromFile(&map_resp_,mapfname.c_str(),resolution,negate,occ_th,free_th, origin, mode);
      }
      catch (std::runtime_error e)
      {
          ROS_ERROR("%s", e.what());
          exit(-1);
      }
      // To make sure get a consistent time in simulation
      ros::Time::waitForValid();
      map_resp_.map.info.map_load_time = ros::Time::now();
      map_resp_.map.header.frame_id = global_frame_id;
      map_resp_.map.header.stamp = ros::Time::now();
      ROS_INFO("Read a %d X %d map @ %.3lf m/cell",
               map_resp_.map.info.width,
               map_resp_.map.info.height,
               map_resp_.map.info.resolution);
      meta_data_message_ = map_resp_.map.info;

      std::string service_name = ns + "/" + desired_name + "/" + "static_map";
      service = n.advertiseService(service_name, &Map::mapCallback, this);

      // Latched publisher for metadata
      std::string metadata_topic_name = ns + "/" + desired_name + "/" + "map_metadata";
      metadata_pub= n.advertise<nav_msgs::MapMetaData>(metadata_topic_name, 1, true);
      metadata_pub.publish( meta_data_message_ );

      // Latched publisher for data
      std::string map_topic_name = ns + "/" + desired_name + "/" + "map";
      map_pub = n.advertise<nav_msgs::OccupancyGrid>(map_topic_name, 1, true);
      map_pub.publish( map_resp_.map );
    }

  private:
    ros::NodeHandle n;
    ros::Publisher map_pub;
    ros::Publisher metadata_pub;
    ros::ServiceServer service;

    /** Callback invoked when someone requests our service */
    bool mapCallback(nav_msgs::GetMap::Request  &req,
                     nav_msgs::GetMap::Response &res )
    {
      // request is empty; we ignore it

      // = operator is overloaded to make deep copy (tricky!)
      res = map_resp_;
      ROS_INFO("Sending map");

      return true;
    }

    /** The map data is cached here, to be sent out to service callers
     */
    nav_msgs::MapMetaData meta_data_message_;
    nav_msgs::GetMap::Response map_resp_;

};

class MultimapServer
{
  public:
    /** Trivial constructor */
    MultimapServer(const std::string& fname)
    {
      std::vector<Map> maps_vector;

      std::ifstream fin(fname.c_str());
      if (fin.fail()) {
        ROS_ERROR("Multimap_server could not open %s.", fname.c_str());
        exit(-1);
      }
#ifdef HAVE_YAMLCPP_GT_0_5_0
      // The document loading process changed in yaml-cpp 0.5.
      YAML::Node doc = YAML::Load(fin);
#else
      YAML::Parser parser(fin);
      YAML::Node doc;
      parser.GetNextDocument(doc);
#endif

      //TODO: This is the main part to modify
      for (YAML::const_iterator namespace_iterator = doc.begin(); namespace_iterator != doc.end(); ++namespace_iterator) {
        // std::cout << namespace_iterator->first.as<std::string>() << "\n";
        std::string global_frame = namespace_iterator->second["global_frame"].as<std::string>();
        // std::cout << namespace_iterator->second["global_frame"].as<std::string>() << "\n";
        YAML::Node maps = namespace_iterator->second["maps"];
        for (YAML::const_iterator maps_iterator = maps.begin(); maps_iterator != maps.end(); ++maps_iterator) {
          std::cout << maps_iterator->second.as<std::string>() << "\n";

          //TODO: Modify resource_retriever package to obtain resolved package path
          // resource_retriever::Retriever r;
          // resource_retriever::MemoryResource resource;
          // try
          // {
          //   resource = r.get(maps_iterator->second.as<std::string>());
          // }
          // catch (resource_retriever::Exception& e)
          // {
          //   ROS_ERROR("Failed to retrieve file: %s", e.what());
          // }
          //
          // std::cout << resource.data.get() << std::endl;

          Map *new_map = new Map(maps_iterator->second.as<std::string>(), namespace_iterator->first.as<std::string>(), maps_iterator->first.as<std::string>(), namespace_iterator->second["global_frame"].as<std::string>());
          maps_vector.push_back(*new_map);
        }
      }

      // for(int i = 0; i < maps_vector.size(); i++)
      // {
      //   std::cout << maps_vector[i].mapfname << std::endl;
      //   std::cout << maps_vector[i].mode << std::endl;
      // }


      // service = n.advertiseService("static_map", &MultimapServer::mapCallback, this);
      // //pub = n.advertise<nav_msgs::MapMetaData>("map_metadata", 1,
      //
      // // Latched publisher for metadata
      // metadata_pub= n.advertise<nav_msgs::MapMetaData>("map_metadata", 1, true);
      // metadata_pub.publish( meta_data_message_ );
      //
      // // Latched publisher for data
      // map_pub = n.advertise<nav_msgs::OccupancyGrid>("map", 1, true);
      // map_pub.publish( map_resp_.map );
    }
  //
  // private:
  //   ros::NodeHandle n;
  //   ros::Publisher map_pub;
  //   ros::Publisher metadata_pub;
  //   ros::ServiceServer service;
  //
  //   /** Callback invoked when someone requests our service */
  //   bool mapCallback(nav_msgs::GetMap::Request  &req,
  //                    nav_msgs::GetMap::Response &res )
  //   {
  //     // request is empty; we ignore it
  //
  //     // = operator is overloaded to make deep copy (tricky!)
  //     res = map_resp_;
  //     ROS_INFO("Sending map");
  //
  //     return true;
  //   }
  //
  //   /** The map data is cached here, to be sent out to service callers
  //    */
  //   nav_msgs::MapMetaData meta_data_message_;
  //   nav_msgs::GetMap::Response map_resp_;
  //
  //   /*
  //   void metadataSubscriptionCallback(const ros::SingleSubscriberPublisher& pub)
  //   {
  //     pub.publish( meta_data_message_ );
  //   }
  //   */

};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "multimap_server", ros::init_options::AnonymousName);
  if(argc != 2)
  {
    ROS_ERROR("%s", USAGE);
    exit(-1);
  }
  std::string fname(argv[1]);

  try
  {
    MultimapServer ms(fname);
    ros::spin();
  }
  catch(std::runtime_error& e)
  {
    ROS_ERROR("multimap_server exception: %s", e.what());
    return -1;
  }

  return 0;
}
