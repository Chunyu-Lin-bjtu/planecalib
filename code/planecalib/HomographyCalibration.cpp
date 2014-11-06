
#include "HomographyCalibration.h"

#include <memory>
#include <ceres/ceres.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include "log.h"
#include "cvutils.h"
#include "CeresParametrization.h"

namespace planecalib {

class HomographyCalibrationError
{
public:
	HomographyCalibrationError(const Eigen::Matrix3d &H_)
		: mH(H_)
    {
    }

	static const int kResidualCount = 2;

	template<typename T>
	void homographyPoint(const Eigen::Matrix<T, 3, 1> &C, Eigen::Matrix<T, 3, 1> &HC) const
	{
		HC[0] = T(mH(0, 0))*C[0] + T(mH(0, 1))*C[1] + T(mH(0, 2))*C[2];
		HC[1] = T(mH(1, 0))*C[0] + T(mH(1, 1))*C[1] + T(mH(1, 2))*C[2];
		HC[2] = T(mH(2, 0))*C[0] + T(mH(2, 1))*C[1] + T(mH(2, 2))*C[2];
	}

	template<typename T>
	void getBasis(const T * const x, Eigen::Matrix<T, 3, 1> &basis1, Eigen::Matrix<T, 3, 1> &basis2) const
	{
		const double kThreshold = 0.1;

		//basis1 = cross(x,C)
		//basis2 = cross(x,basis1)
		//Check that the point we use is not colinear with x
		const double x1val = CeresUtils::ToDouble(x[1]);
		const double x2val = CeresUtils::ToDouble(x[2]);
		if (x1val > kThreshold || x1val < -kThreshold || x2val > kThreshold || x2val < -kThreshold)
		{
			//Use C=[1,0,0]
			basis1[0] = T(0);
			basis1[1] = x[2];
			basis1[2] = -x[1];

			basis2[0] = -(x[1] * x[1] + x[2] * x[2]);
			basis2[1] = x[0] * x[1];
			basis2[2] = x[0] * x[2];
		}
		else
		{
			//Use C=[0,1,0]
			basis1[0] = -x[2];
			basis1[1] = T(0);
			basis1[2] = x[0];

			basis2[0] = x[0] * x[1];
			basis2[1] = -(x[0] * x[0] + x[2] * x[2]);
			basis2[2] = x[1] * x[2];
		}
		//basis1.normalize();
		//basis2.normalize();
	}

	//Homography is in row-major order
    template<typename T>
	bool operator()(const T * const alpha_, const T * const pp, const T * const normal, T *residuals) const
    {
		const T &alpha = *alpha_;
		const T &u0 = pp[0];
		const T &v0 = pp[1];

		Eigen::Matrix<T, 3, 3> K;
		K <<
			alpha, T(0.0), u0,
			T(0.0), alpha, v0,
			T(0.0), T(0.0), T(1.0);

		T alphaInv = T(1.0) / alpha;
		Eigen::Matrix<T, 3, 3> Kinv;
		Kinv <<
			alphaInv, T(0.0), -u0*alphaInv,  
			T(0.0), alphaInv, -v0*alphaInv,
			T(0.0), T(0.0), T(1.0);

		Eigen::Matrix<T, 3, 1> C1;
		Eigen::Matrix<T, 3, 1> C2;

		getBasis(normal, C1, C2);
		C1 = K*C1;
		C2 = K*C2;

		Eigen::Matrix<T, 3, 1> HC1;
		homographyPoint(C1, HC1);

		Eigen::Matrix<T, 3, 1> KiHC1;
		KiHC1 = Kinv*HC1;
		
		Eigen::Matrix<T, 3, 1> HC2;
		homographyPoint(C2, HC2);

		Eigen::Matrix<T, 3, 1> KiHC2;
		KiHC2 = Kinv*HC2;

		T norm1 = KiHC1.norm();
		T norm2 = KiHC2.norm();

		//Residuals
		residuals[0] = KiHC1.dot(KiHC2) / (norm1*norm2);
		residuals[1] = T(1)-norm2/norm1;
        return true;
    }

private:
	const Eigen::Matrix3d &mH;
};

void HomographyCalibration::calibrate(const std::vector<Eigen::Matrix3fr> &H, const Eigen::Vector2i &imageSize)
{
	int hcount = H.size();

	//Adjust for principal point
	Eigen::Matrix3fr T, Ti;
	float halfWidth = (float)imageSize.x()/2;
	float halfHeight = (float)imageSize.y() / 2;
	T << 1, 0, -halfWidth, 0, 1, -halfHeight, 0, 0, 1;
	Ti << 1, 0, +halfWidth / 2, 0, 1, +halfHeight / 2, 0, 0, 1;

	std::vector<Eigen::Matrix3d> Ht(hcount);
	for (int i = 0; i < hcount; i++)
	{
		Ht[i] = (T * H[i] * Ti).cast<double>();
		//Ht[i] /= Ht[i].determinant();
	}

	//Assume normal is [0,0,1] and find focal length
	Eigen::VectorXd A(2 * hcount), b(2 * hcount);
	for (int i = 0; i < hcount; i++)
	{
		auto &Hti = Ht[i];

		//Note: Hti must be column-major!
		A[2 * i] = -Hti(2)*Hti(5);
		b[2 * i] = Hti(0)*Hti(3) + Hti(1)*Hti(4);
		A[2 * i + 1] = Hti(5)*Hti(5) - Hti(2)*Hti(2);
		b[2 * i + 1] = -(Hti(3)*Hti(3) - Hti(0)*Hti(0) + Hti(4)*Hti(4) - Hti(1)*Hti(1));
	}

	double alpha = sqrt(A.dot(b) / (A.dot(A))); //alpha = (pinv(A)*b)^0.5;
	mInitialAlpha = alpha;

	//Non-linear minimization
	ceres::Problem problem;
	ceres::LossFunction *lossFunc = new ceres::CauchyLoss(3.0);

	Eigen::Vector2d pp(0,0); //Assume principal point is in the middle
	mNormal << 0, 0, 1; //Assume reference camera is perependicular to plane.

	problem.AddParameterBlock(&alpha, 1);
	problem.AddParameterBlock(pp.data(), 2);
	//problem.SetParameterBlockConstant(pp.data());
	problem.AddParameterBlock(mNormal.data(), 3, new Fixed3DNormParametrization(1));
	//problem.SetParameterBlockConstant(mNormal.data());
	for (int i = 0; i<hcount; i++)
	{
		problem.AddResidualBlock(
			new ceres::AutoDiffCostFunction<HomographyCalibrationError, HomographyCalibrationError::kResidualCount, 1, 2, 3>(
			new HomographyCalibrationError(Ht[i])),
			lossFunc, &alpha,pp.data(),mNormal.data());
	}

	ceres::Solver::Options options;
	options.gradient_tolerance = 1e-25;
	options.parameter_tolerance = 1e-25;
	options.function_tolerance = 1e-25;
	options.linear_solver_type = ceres::DENSE_QR;
	options.minimizer_progress_to_stdout = false;

	ceres::Solver::Summary summary;

	ceres::Solve(options, &problem, &summary);
	MYAPP_LOG << summary.FullReport();

	MYAPP_LOG << "Initial alpha=" << alpha << ", final=" << alpha << "\n";

	//Build final K
	mK << (float)alpha, 0, (float)pp.x(), 0, (float)alpha, (float)pp.y(), 0, 0, 1;
}

} 
