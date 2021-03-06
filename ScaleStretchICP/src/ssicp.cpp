#include "ssicp.h"

#include <cmath>
#include <algorithm>
#include <climits>

#include <Eigen/dense>
#include "kdtree.h"
#include <igl/readOBJ.h>
#include <igl/writeOBJ.h>
#include <igl/slice_mask.h>

void EigenvaluesAndEigenvectors(const Eigen::Matrix3d &A, std::vector<double> &eigenvalues,
  std::vector<Eigen::Vector3d> &eigenvectors)
{
  Eigen::EigenSolver<Eigen::Matrix3d> es(A);
  eigenvalues.resize(3), eigenvectors.resize(3);
  for (size_t i = 0; i < 3; ++i)
  {
    eigenvalues[i] = es.eigenvalues()[i].real();
    Eigen::Vector3cd ev = es.eigenvectors().col(i);
    for (size_t j = 0; j < 3; ++j)
      eigenvectors[i](j) = ev(j).real();
    eigenvectors[i].normalize();
  }

  // sort eigenvalues with eigenvectors
  for (size_t i = 0; i < 3; ++i)
    for (size_t j = i + 1; j < 3; ++j)
      if (eigenvalues[i] > eigenvalues[j])
      {
        std::swap(eigenvalues[i], eigenvalues[j]);
        std::swap(eigenvectors[i], eigenvectors[j]);
      }
}

SSICP_PUBLIC void SSICP::Initialize(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Y,
  double &s, double &a, double &b, Eigen::Matrix3d &R, Eigen::RowVector3d &T)
{
  // calculate initial values for s, a and b
  Eigen::RowVector3d x_c = X.colwise().sum() / static_cast<double>(X.rows());
  Eigen::RowVector3d y_c = Y.colwise().sum() / static_cast<double>(Y.rows());
  Eigen::MatrixXd X_tilde(X), Y_tilde(Y);
  X_tilde.rowwise() -= x_c, Y_tilde.rowwise() -= y_c;
  Eigen::Matrix3d M_X = (X_tilde.transpose().eval()) * X_tilde;
  Eigen::Matrix3d M_Y = (Y_tilde.transpose().eval()) * Y_tilde;

  std::vector<double> evax, evay;
  std::vector<Eigen::Vector3d> evex, evey;
  EigenvaluesAndEigenvectors(M_X, evax, evex);
  EigenvaluesAndEigenvectors(M_Y, evay, evey);

  a = std::numeric_limits<double>::max(), b = std::numeric_limits<double>::lowest(), s = 0;
  for (size_t i = 0; i < 3; ++i)
  {
    double v = sqrt(evay[i] / evax[i]);
    a = std::min(a, v);
    b = std::max(b, v);
    s += v / 3;
  }

  // intial values for R
  /*
  Eigen::Matrix3d P, Q;
  for (size_t i = 0; i < 3; ++i)
    for (size_t j = 0; j < 3; ++j)
    {
      P(i, j) = evex[j](i);
      Q(i, j) = evey[j](i);
    }
  R = Q * (P.transpose());
  */
  // before fixing the orientation of the main axises
  // we use identity for initial R
  R = Eigen::Matrix3d::Identity();

  // intial values for T
  T = y_c - x_c;
}

SSICP_PUBLIC void SSICP::Iterate(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Y, const double &a, const double &b, const double& trim_thre,
  const double &epsilon, double &s, Eigen::Matrix3d &R, Eigen::RowVector3d &T)
{
  size_t counter = 0;
  double last_error = std::numeric_limits<double>::max();
  for (;;)
  {
    ++counter;
    printf("Iteration %zu:\n", counter);

    Eigen::MatrixXd Z = FindCorrespondeces(X, Y, s, R, T);
	Eigen::MatrixXd filtered_X, filtered_Z;
	if(trim_thre > 0)
	{
		Eigen::Array<bool, Eigen::Dynamic, 1> feature_mask(X.rows());
		for (int i = 0; i < Z.rows(); ++i)
		{
			if ((X.row(i) - Z.row(i)).norm() > trim_thre)
			{
				feature_mask(i) = false;
			}
			else
			{
				feature_mask(i) = true;
			}
		}
		filtered_X = igl::slice_mask(X, feature_mask, 1);
		filtered_Z = igl::slice_mask(Z, feature_mask, 1);
	}
	else
	{
		filtered_X = X;
		filtered_Z = Z;
	}

    double error = ComputeError(filtered_X, filtered_Z, s, R, T);
    printf("Error after the first step: %lf\n", error);
    FindTransformation(filtered_X, filtered_Z, a, b, s, R, T);
    error = ComputeError(filtered_X, filtered_Z, s, R, T);
    printf("Error after the second step: %lf\n", error);
    
    if (counter > 1)
    {
      double theta = 1 - error / last_error;
      if (theta < epsilon) break;
    }
    last_error = error;
  }
  printf("Alignment Finished!\n");
}

SSICP_PUBLIC Eigen::MatrixXd SSICP::FindCorrespondeces(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Y, const double &s,
  const Eigen::Matrix3d &R, const Eigen::RowVector3d &T)
{
  // use kdtree to calculate nearest points
  kdtree *ptree = kd_create(3);
  char *data = new char('a');
  for (int i = 0; i < Y.rows(); ++i)
    kd_insert3(ptree, Y(i, 0), Y(i, 1), Y(i, 2), data);

  Eigen::MatrixXd SRXT = s * X * R.transpose();
  SRXT.rowwise() += T;

  Eigen::MatrixXd Z(X.rows(), X.cols());
  for (int i = 0; i < X.rows(); ++i)
  {
    kdres *presults = kd_nearest3(ptree, SRXT(i, 0), SRXT(i, 1), SRXT(i, 2));
    while (!kd_res_end(presults))
    {
      double pos[3];
      kd_res_item(presults, pos);
      for (int j = 0; j < 3; ++j)
        Z(i, j) = pos[j];
      kd_res_next(presults);
    }
    kd_res_free(presults);
  }
  free(data);
  kd_free(ptree);

  return Z;
}

SSICP_PUBLIC bool SSICP::FindTransformation(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Z, const double &a,
  const double &b, double &s, Eigen::Matrix3d &R, Eigen::RowVector3d &T)
{
  // calculate X_tilde and Z_tilde
  size_t rows = X.rows(), cols = X.cols();
  Eigen::RowVector3d x_c = X.colwise().mean();
  Eigen::RowVector3d z_c = Z.colwise().mean();
  Eigen::MatrixXd X_tilde(X), Z_tilde(Z);
  X_tilde.rowwise() -= x_c, Z_tilde.rowwise() -= z_c;

  // update R using SVD decomposition
  Eigen::Matrix3d H = (X_tilde.transpose().eval()) * Z_tilde;
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Matrix3d U = svd.matrixU(), V = svd.matrixV();
  if ((V * (U.transpose())).determinant() > 0)
    R = V * (U.transpose());
  else
  {
	  if(abs(svd.singularValues().asDiagonal().toDenseMatrix().determinant()) > 1e-10)
	  {
		  std::cout << "Negative determinant of VU^t" << std::endl;
		  std::cout << svd.singularValues() << std::endl;
		  return false;
	  }

    Eigen::Matrix3d I;
    I << 1, 0, 0,
      0, 1, 0,
      0, 0, -1;
    R = V * I * (U.transpose());
  }

  // update s
  double num = (Z_tilde.array() * (X_tilde * R.transpose()).array()).sum();
  double den = X_tilde.squaredNorm();
  s = num / den;
  if (s < a) s = a;
  if (s > b) s = b;
  T = z_c - s * x_c * (R.transpose());
  return true;
}

SSICP_PUBLIC double SSICP::ComputeError(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Z, const double &s,
	const Eigen::Matrix3d &R, const Eigen::RowVector3d &T)
{
	Eigen::MatrixXd E = s * X * R.transpose() - Z;
	E.rowwise() += T;
	double e = E.squaredNorm();
	return e;
}

SSICP_PUBLIC void SSICP::OutputParameters(const double &s, const double &a, const double &b, const Eigen::MatrixXd &R,
  const Eigen::RowVector3d T)
{
  std::cout << "Scale: " << s << " in [" << a << ", " << b << "]" << std::endl;
  std::cout << "Rotation: " << std::endl;
  std::cout << R << std::endl;
  std::cout << "Translation: " << std::endl;
  std::cout << T << std::endl;
}

SSICP_PUBLIC Eigen::MatrixXd SSICP::GetTransformed(const Eigen::MatrixXd &X, const double &s, const Eigen::MatrixX3d &R,
  const Eigen::RowVector3d &T)
{
  Eigen::MatrixXd A = s * X * R.transpose();
  A.rowwise() += T;
  return A;
}

SSICP_PUBLIC Eigen::MatrixXd SSICP::Align(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Y)
{
  double s, a, b, epsilon = 0.001, trim_thre = -1;
  Eigen::Matrix3d R;
  Eigen::RowVector3d T;
  Initialize(X, Y, s, a, b, R, T);
#ifndef NDEBUG
  OutputParameters(s, a, b, R, T);
#endif // NDEBUG
  Iterate(X, Y, a, b, trim_thre, epsilon, s, R, T);
  return GetTransformed(X, s, R, T);
}


SSICP_PUBLIC Eigen::Matrix4d SSICP::GetOptimalTrans(const Eigen::MatrixXd &X, const Eigen::MatrixXd &Y, const double& trim_thre,
	const double &epsilon)
{
	double s = 1, a = 0.9, b = 1.1;
	Eigen::Matrix3d R;
	Eigen::RowVector3d T;
	R.setIdentity();
	T.setZero();

	Iterate(X, Y, a, b, trim_thre, epsilon, s, R, T);
	Eigen::Matrix4d trans_mat;
	trans_mat.setZero();
	trans_mat.block<3, 3>(0, 0) = R;
	trans_mat.block<3, 3>(0, 0) = s * R;
	trans_mat.block<3, 1>(0, 3) = T.transpose();
	trans_mat(3, 3) = 1;

	return trans_mat;
}