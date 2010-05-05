// __BEGIN_LICENSE__
// __END_LICENSE__

#include <gtest/gtest.h>

#include <boost/random.hpp>

#include <vw/Camera/PinholeModel.h>
#include <vw/Camera/CameraGeometry.h>
#include <vw/Math/EulerAngles.h>
#include <test/Helpers.h>

using namespace vw;
using namespace vw::camera;

TEST( CameraGeometry, PinholeLinearSolve ) {
  Matrix<double,3,3> pose = math::euler_to_rotation_matrix(1.3,2.0,-.7,"xyz");

  // Create an imaginary 1000x1000 pixel imager
  PinholeModel pinhole( Vector3(0,4,-10),
                        pose, 600, 700,
                        500, 500,
                        NullLensDistortion() );
  std::vector<Vector<double> > world_m, image_m;

  // Building random measurements
  boost::minstd_rand random_gen(42u);
  boost::normal_distribution<double> normal(0,20);
  boost::variate_generator<boost::minstd_rand&,
    boost::normal_distribution<double> > generator( random_gen, normal );
  for ( uint8 i = 0; i < 6; i++ ) {
    Vector3 point( generator(), generator(), generator()+60.0 );
    world_m.push_back( Vector4(point[0],point[1],point[2],1) );
    Vector2 pixel = pinhole.point_to_pixel(point);
    image_m.push_back( Vector3(pixel[0],pixel[1],1) );
  }

  // Building P Matrix
  CameraMatrixFittingFunctor fitfunc;
  Matrix<double> P = fitfunc(world_m,image_m);
  ASSERT_EQ( P.rows(), 3 );
  ASSERT_EQ( P.cols(), 4 );

  // Testing to see that it matches pinhole
  for ( uint8 i = 0; i < 10; i++ ) {
    Vector4 point( generator(), generator(), generator()+60.0, 1.0 );
    Vector3 p_result = P*point;
    p_result /= p_result[2];
    Vector2 cam_result = pinhole.point_to_pixel(subvector(point,0,3));
    EXPECT_VECTOR_NEAR( subvector(p_result,0,2),
                        cam_result, 1e-5 );
  }
}

TEST( CameraGeometry, PinholeIterSolve ) {
  Matrix<double,3,3> pose = math::euler_to_rotation_matrix(1.3,2.0,-.7,"xyz");

  // Create an imaginary 1000x1000 pixel imager
  PinholeModel pinhole( Vector3(0,4,-30),
                        pose, 600, 700,
                        300, 300,
                        NullLensDistortion() );
  std::vector<Vector<double> > world_m, image_m;

  // Building random measurements
  {
    boost::minstd_rand random_gen(13u);
    boost::uniform_real<double> uniform(0,600);
    boost::variate_generator<boost::minstd_rand&,
      boost::uniform_real<double> > generator( random_gen, uniform );
    for ( uint8 i = 0; i < 50; i++ ) {
      Vector2 pixel( generator(), generator() );
      Vector3 point = pinhole.camera_center() + (generator()/6+30) * pinhole.pixel_to_vector(pixel);
      world_m.push_back( Vector4(point[0],point[1],point[2],1) );
      image_m.push_back( Vector3(pixel[0],pixel[1],1) );
    }
  }

  // Adding Noise
  {
    boost::minstd_rand random_gen(42u);
    boost::normal_distribution<double> normal(0,0.3);
    boost::variate_generator<boost::minstd_rand&,
      boost::normal_distribution<double> > generator( random_gen, normal );
    for ( uint8 i = 0; i < 50; i++ ) {
      Vector4 noise(generator(),generator(),generator(),0);
      if ( norm_2(noise) > 1 )
        noise /= norm_2(noise);
      world_m[i] += noise;
    }
  }

  // Building P Matrix
  CameraMatrixFittingFunctor fitfunc;
  Matrix<double> P = fitfunc(world_m,image_m);
  ASSERT_EQ(P.rows(),3);
  ASSERT_EQ(P.cols(),4);

  // Testing to see that it matches pinhole
  for ( uint8 i = 0; i < 50; i++ ) {
    Vector3 p_result = P*world_m[i];
    p_result /= p_result[2];
    EXPECT_VECTOR_NEAR( image_m[i], p_result, 10 );
  }
}


