#include "test.h"

#include <stdexcept>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "baldr/graphreader.h"
#include "baldr/graphid.h"
#include "meili/traffic_segment_matcher.h"

using namespace valhalla;

namespace {

  //here we hijack a couple of methods and save off some state while we're at it
  //this way the standard calling pattern used from the outside is the same as in the test
  //but we now have the internal state so we can see what is going on at more detail
  class testable_matcher : public meili::TrafficSegmentMatcher {
   public:
    using meili::TrafficSegmentMatcher::TrafficSegmentMatcher;

    std::list<std::vector<meili::interpolation_t> > interpolate_matches(const std::vector<meili::MatchResult>& r,
      const std::shared_ptr<meili::MapMatcher>& m) const override {
      matches = r;
      matcher = m;
      interpolations = meili::TrafficSegmentMatcher::interpolate_matches(r, m);
      return interpolations;
    }

    std::vector<meili::traffic_segment_t> form_segments(const std::list<std::vector<meili::interpolation_t> >& i,
      baldr::GraphReader& r) const override {
      segments = meili::TrafficSegmentMatcher::form_segments(i, r);
      return segments;
    }

    mutable std::vector<valhalla::meili::MatchResult> matches;
    mutable std::shared_ptr<meili::MapMatcher> matcher;
    mutable std::list<std::vector<meili::interpolation_t> > interpolations;
    mutable std::vector<meili::traffic_segment_t> segments;
  };

  //TODO: build the test tiles in the test, need to move traffic association into library to do that
  //currently all the logic is in the application

  using ots_t = meili::traffic_segment_t; //id,start time,start idx,end time,end idx,length
  using ots_matches_t = std::vector<ots_t>;
  using sid_t = baldr::GraphId;
  std::vector<std::pair<std::string, ots_matches_t> > test_cases {
    //partial, partial
    std::make_pair(R"({"trace":[{"lon":-76.376045,"lat":40.539207,"time":0},{"lon":-76.357056,"lat":40.541309,"time":1}]})",
      ots_matches_t{ots_t{sid_t(0),-1,0,.5f,0,-1}, ots_t{sid_t(0),.5f,0,-1,1,-1}}),
    //partial, full, partial
    std::make_pair(R"({"trace":[{"lon":-76.376045,"lat":40.539207,"time":0},{"lon":-76.351089,"lat":40.541504,"time":3}]})",
      ots_matches_t{ots_t{sid_t(0),-1,0,1.f,0,-1}, ots_t{sid_t(0),1.f,0,2.5f,0,1000}, ots_t{sid_t(0),2.5f,0,-1,1,-1}}),
    //partial, full, full, full
    std::make_pair(R"({"trace":[{"lon":-76.38126,"lat":40.55602,"time":0},{"lon":-76.35784,"lat":40.56786,"time":6}]})",
      ots_matches_t{ots_t{sid_t(0),-1,0,.5f,0,-1}, ots_t{sid_t(0),.5f,0,1.f,0,200}, ots_t{sid_t(0),1.f,0,3.5f,0,1000}, ots_t{sid_t(0),3.5f,0,6.f,1,1000}}),
    //full, full, partial
    std::make_pair(R"({"trace":[{"lon":-76.35784,"lat":40.56786,"time":0},{"lon":-76.38126,"lat":40.55602,"time":6}]})",
      ots_matches_t{ots_t{sid_t(0),0.f,0,2.5f,0,1000}, ots_t{sid_t(0),2.5f,0,5.f,0,1000}, ots_t{sid_t(0),5.f,0,-1,0,-1}}),

    //TODO: add test where its all full segments
    //TODO: add test where you are on at the start of a segment, you get off on a small road in between,
    //but come back on again before the segment ends, this segment should be seen twice in output as partials
    //TODO: add test where you are consecutively in the same spot at different times, ie you aren't moving
    //TODO: add test where there is discontinuity in matches so it has to do two sets of matches
    //TODO: add test where intermediate trace points dont get matches, this causes their times to not be used
    //for interpolation but we can still get valid segments on the edges for the entire trace
    //TODO: add a test where you enter a segment, leave it and come back onto it where it starts, via loop,
    //then finish it and you should see partial, then full and the full should not count the length of the partial in it
  };

  void test_matcher() {
    //fake config
    std::stringstream conf_json; conf_json << R"({
      "mjolnir":{"tile_dir":"test/traffic_matcher_tiles"},
      "meili":{"mode":"auto","grid":{"cache_size":100240,"size":500},
               "default":{"beta":3,"breakage_distance":10000,"geometry":false,"gps_accuracy":5.0,
                          "interpolation_distance":10,"max_route_distance_factor":3,"max_search_radius":100,
                          "route":true,"search_radius":50,"sigma_z":4.07,"turn_penalty_factor":200}}
    })";
    boost::property_tree::ptree conf;
    boost::property_tree::read_json(conf_json, conf);

    //find me a find, catch me a catch
    testable_matcher matcher(conf);

    //some edges should have no matches and most will have no segments
    for(const auto& test_case : test_cases) {
      auto json = matcher.match(test_case.first);
      std::stringstream json_ss; json_ss << json;
      boost::property_tree::ptree answer;
      boost::property_tree::read_json(json_ss, answer);

      const auto& a_segs = test_case.second;
      const auto& b_segs = matcher.segments;
      if(a_segs.size() != b_segs.size())
        throw std::logic_error("wrong number of segments matched");
      for(size_t i = 0; i < test_case.second.size(); ++i) {
        const auto& a = test_case.second[i];
        const auto& b = matcher.segments[i];
        if(a.begin_shape_index != b.begin_shape_index)
          throw std::logic_error("begin_shape_index mismatch");
        if(a.end_shape_index != b.end_shape_index)
          throw std::logic_error("end_shape_index mismatch");
        if(std::fabs(a.start_time - b.start_time) > .25)
          throw std::logic_error("start time is out of tolerance");
        if(std::fabs(a.end_time - b.end_time) > .25)
          throw std::logic_error("end time is out of tolerance");
        if(std::fabs(a.length - b.length) > 50)
          throw std::logic_error("length is out of tolerance");
      }
    }

  }

}

int main() {
  test::suite suite("traffic matcher");

  suite.test(TEST_CASE(test_matcher));

  return suite.tear_down();
}
