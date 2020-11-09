#include <unistd.h>
#include <cmath>

#include <igl/read_triangle_mesh.h>
#include <iostream>

#include "igl/readPLY.h"
#include "igl/writePLY.h"
#include "nanoflann.hpp"

#include "cxxopts.hpp"
#include <algorithm>
#include "depth_potential.h"


// compute
#include <igl/avg_edge_length.h>
#include <igl/grad.h>
#include <igl/adjacency_matrix.h>

#include "util.h"

void fix_grad(Eigen::SparseMatrix<double> &G, double threshold=1e9)
{
  // A hack to remove extreme values
  for(int k=0; k<G.outerSize(); ++k)
  {
    for(typename Eigen::SparseMatrix<double>::InnerIterator it (G,k); it; ++it)
    {
      if( G.coeffRef(it.row(),it.col()) < -threshold ||
          G.coeffRef(it.row(),it.col()) >  threshold ||
         !std::isfinite(G.coeffRef(it.row(),it.col())) )

         G.coeffRef(it.row(),it.col()) = 0.0;
    }
  }  
}

void smooth_grad(
  const Eigen::VectorXd       &Fun, 
  Eigen::SparseMatrix<double> &G,
  Eigen::SparseMatrix<double> &F2V,
  Eigen::SparseMatrix<double> &S, 
  int iter, 
  Eigen::MatrixXd             &gFun) 
{
  Eigen::MatrixXd fgFun = Eigen::Map<const Eigen::MatrixXd>((G*Fun).eval().data(), G.rows()/3, 3); // gradient on the face
  gFun = F2V * fgFun; // average across adjacent faces

  // apply smoothing to the gradient
  for(int i=0;i<iter;++i) 
    gFun = (S * gFun).eval();
}

void face_to_vertex(const Eigen::MatrixXi  &F,
 const Eigen::MatrixXd& V, 
 Eigen::SparseMatrix<double> &M)
{
  std::vector<Eigen::Triplet<double> > Mijv;
  Mijv.reserve( F.rows()*3 );
  M.resize(V.rows(), F.rows());
  for(int f = 0;f<F.rows();f++)
  {
    for(int d = 0;d<3;d++)
    {
      Mijv.emplace_back(F(f,d), f, 1.0);
    }
  }
  M.setFromTriplets(Mijv.begin(), Mijv.end());

  // normalize (make sure rowwise sum is 1.0)
  M = Eigen::SparseMatrix<double>(( M * Eigen::VectorXd::Ones(F.rows())).cwiseInverse().asDiagonal()) * M;
}

template <typename DerivedC, 
          typename DerivedD
          > 
void find_correspondence(
    const Eigen::PlainObjectBase<DerivedC> & src,
    const Eigen::PlainObjectBase<DerivedC> & trg,
    Eigen::PlainObjectBase<DerivedD>       & idx  ) 
{
  const int leaf_max_size = 10;

  using kd_tree_t = nanoflann::KDTreeEigenMatrixAdaptor< typename Eigen::PlainObjectBase<DerivedC>, 3, nanoflann::metric_SO3 > ;
  idx.resize(src.rows());

  kd_tree_t kdtree(trg.cols() , trg, leaf_max_size );
  kdtree.index->buildIndex();
  for(auto i=0;i<src.rows();++i)
  {
    Eigen::Array< typename DerivedD::Scalar,1,1> ret_indexes;
    Eigen::Array< typename DerivedC::Scalar,1,1> out_dists_sqr;

    nanoflann::KNNResultSet< typename DerivedC::Scalar,typename DerivedD::Scalar,typename DerivedD::Scalar > resultSet(1);
    resultSet.init( ret_indexes.data(), out_dists_sqr.data() );

    Eigen::Array< typename DerivedC::Scalar,-1,1 > row_sph = src.row(i); 
    kdtree.index->findNeighbors(resultSet, row_sph.data(), nanoflann::SearchParams());
    idx(i) = ret_indexes(0);
  }
};

int main(int argc, char *argv[])
{
  cxxopts::Options options(argv[0], "Resample field");

  options
      .positional_help("<source> <target>")
      .show_positional_help();
  
  options.add_options()
    ("v,verbose", "Verbose output",     cxxopts::value<bool>()->default_value("false"))
    ("s,source", "Source mesh ",        cxxopts::value<std::string>())
    ("t,target", "Target mesh ",        cxxopts::value<std::string>())
    ("i,input",  "Input  field (csv) ", cxxopts::value<std::string>())
    ("chess",    "Generate input data as chessboard ", cxxopts::value<bool>()->default_value("false"))
    ("o,output", "Output field (csv) ", cxxopts::value<std::string>())

    ("a,alpha", "Alpha parameter for surface depth", cxxopts::value<double>()->default_value("0.03"))
    ("step",    "Step size", cxxopts::value<double>()->default_value("0.1"))
    ("lambda",  "Regularization lambda", cxxopts::value<double>()->default_value("1.0"))
    ("iter",    "Iter number", cxxopts::value<int>()->default_value("1000"))

    ("SO3",      "Use SO3 metric (angular distance)", cxxopts::value<bool>()->default_value("false"))

    ("clobber", "Clobber output file ", cxxopts::value<bool>()->default_value("false"))

    ("help", "Print help") ;
  
  options.parse_positional({"source", "target" });
  auto par = options.parse(argc, argv);

  double alpha           = par["alpha"].as<double>();
  double demons_step     = par["step"].as<double>();
  double lambda          = par["lambda"].as<double>();
  int    demons_iter     = par["iter"].as<int>();;
  bool   verbose         = par["verbose"].as<bool>();

  if( par.count("source") && 
      par.count("target") && 
      par.count("output") )
  {
    if ( !par["clobber"].as<bool>() && 
         !access( par["output"].as<std::string>().c_str(), F_OK)) {
      std::cerr << par["output"].as<std::string>()<<" Exists!"<<std::endl;
      return 1;
    }

    Eigen::MatrixXd V1,N1,UV1,D1;
    Eigen::MatrixXi F1,E1;
    std::vector<std::string> header1;

    Eigen::MatrixXd V2,N2,UV2,D2;
    Eigen::MatrixXi F2,E2;
    std::vector<std::string> header2;

    Eigen::MatrixXd D;
    std::vector<std::string> header;

    if(! igl::readPLY(par["source"].as<std::string>(), V1, F1, E1, N1, UV1, D1, header1))
    {
      std::cerr<<"Error reding ply:"<<par["source"].as<std::string>()<<std::endl;
      return 1;
    }

    if(! igl::readPLY(par["target"].as<std::string>(), V2, F2, E2, N2, UV2, D2, header2) )
    {
      std::cerr<<"Error reding ply:"<<par["target"].as<std::string>()<<std::endl;
      return 1;
    }

    if(verbose)
    {
      std::cout << "Source Mesh 1:" << argv[1] << std::endl;
      std::cout << " Vertices: " << V1.rows() << "x"<< V1.cols() << std::endl;
      std::cout << " Faces:    " << F1.rows() << "x"<< F1.cols() << std::endl;
      std::cout << " Data:     " << D1.rows() << "x"<< D1.cols() << std::endl;
      std::cout << " Header:   ";
      for(auto i: header1)
          std::cout<<i<<"\t";
      std::cout<<std::endl;

      std::cout << "Reference Mesh 2:" << argv[2] << std::endl;
      std::cout << " Vertices: " << V2.rows() << "x"<< V2.cols() << std::endl;
      std::cout << " Faces:    " << F2.rows() << "x"<< F2.cols() << std::endl;
      std::cout << " Data:     " << D2.rows() << "x"<< D2.cols() << std::endl;
      std::cout << " Header:   ";
      for(auto i: header2)
          std::cout<<i<<"\t";
      std::cout<<std::endl;
    }

    Eigen::VectorXd dp1,dp2;
    
    if(!depth_potential(V1,F1,alpha,dp1)) {
        std::cerr<<"Solving failed for Mesh 1"<<std::endl;
        return 1;
    }

    if(!depth_potential(V2,F2,alpha,dp2)) {
        std::cerr<<"Solving failed for Mesh 2"<<std::endl;
        return 1;
    }

    // normalize - subtract mean , devide by std
    dp1.array() -= dp1.mean(); dp1.array() /= sqrt( (dp1.array().pow(2).sum())/(dp1.size()-1) );
    dp2.array() -= dp2.mean(); dp2.array() /= sqrt( (dp2.array().pow(2).sum())/(dp2.size()-1) );

    Eigen::MatrixXd  PT1, PT2;
    Eigen::MatrixXd  sph1, sph2;

    // convert spherical coordinates to x,y,z on sphere 
    if( extract_psi_the(header1, D1, PT1) &&
        extract_psi_the(header2, D2, PT2) )
    {
      sph_to_xyz(PT1, sph1);
      sph_to_xyz(PT2, sph2);
    } else {
      std::cerr<<"Can't get spherical coordinates!"<<std::endl;
      return 1;
    }

    // registration parameters
    int    smooth_gradients = 50;
    double smooth_sigma  = 1.0;
    double demons_sigma_x =  1.0;
    double demons_sigma_x2= demons_sigma_x*demons_sigma_x;

    int smooth_partial_grad = 20;
    int smooth_update       = 20;
    //int smooth_cost      = 10;
    double conv_tol      = 1e-5;
    int conv_iter        = 10;
    int grad_update_iter = 40;

    // avg edge length on embedding
    double dx_X1 = igl::avg_edge_length(sph1, F1); 
    double dx_X2 = igl::avg_edge_length(sph2, F2); 
    if(verbose)
      std::cout<<"Average edge lengths (sphere):"<<dx_X1<<" "<<dx_X2<<std::endl;

    Eigen::SparseMatrix<double> Wsource,Wtarget;
    igl::adjacency_matrix(F1,Wsource);
    igl::adjacency_matrix(F2,Wtarget);

    // create smoothing matrix (es)
    Eigen::SparseMatrix<double> Lsource = Eigen::SparseMatrix<double>((Wsource * Eigen::VectorXd::Ones(Wsource.cols())).cwiseInverse().asDiagonal()) * Wsource;
    Eigen::SparseMatrix<double> Ltarget = Eigen::SparseMatrix<double>((Wtarget * Eigen::VectorXd::Ones(Wtarget.cols())).cwiseInverse().asDiagonal()) * Wtarget;

    // mapping from face to vertex
    //create sparse matrix mapping faces to vertices (averaging)
    Eigen::SparseMatrix<double> F2V1, F2V2;
    face_to_vertex(F1, V1, F2V1);
    face_to_vertex(F2, V2, F2V2);

    Eigen::MatrixXd C1s = dp1;
    Eigen::MatrixXd C2s = dp2;


    // gradient of the target feature
    Eigen::MatrixXd dC1s,dC2s;
    Eigen::VectorXi match1(V1.rows());
    Eigen::VectorXi match2(V2.rows());

    // Gradient operators
    Eigen::SparseMatrix<double> Gsource, Gtarget;

    Eigen::MatrixXd  sph1_orig=sph1;

    //Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double > > solver;
    Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double> > solver;

    for(int i=0; i<demons_iter; ++i)
    {
  //    igl::grad(sph1, F1, Gsource); fix_grad(Gsource,1e6);
      igl::grad(sph2, F2, Gtarget); fix_grad(Gtarget,1e6);

      // calculate smooth gradient on the mesh
//      smooth_grad(C1s, Gsource, F2V1, Lsource, 10, dC1s);
      smooth_grad(C2s, Gtarget, F2V2, Ltarget, smooth_partial_grad, dC2s);

      // find matching vertices
      find_correspondence(sph1, sph2, match1);
      //find_correspondence(sph2, sph1, match2);

      Eigen::VectorXd diff1 = Eigen::VectorXd::NullaryExpr(C1s.rows(),
        [ &C1s, &C2s, &match1 ](auto r) { return C1s(r) - C2s(match1(r)); });
      
      double cost_fw = sqrt( diff1.squaredNorm()/diff1.rows() );

      Eigen::MatrixXd avg_grad1 = Eigen::MatrixXd::NullaryExpr(C1s.rows(), dC2s.cols(),
          [&dC1s, &dC2s, &match1](auto r, auto c) { return dC2s(match1(r), c); }); 
      //Eigen::MatrixXd avg_grad1 = dC1s;

      Eigen::ArrayXd normg1 = avg_grad1.rowwise().stableNorm();
      Eigen::ArrayXd scale1 = diff1.array() / (normg1.array() + diff1.array().pow(2) * lambda  ); // % levenberg-marquart descent

      // % vanishing gradients
      // vanishing_grad_fw = (normg1 + diff1.array().pow(2) < 1e-6
      // scale[ vanishing_grad_fw ] = 0.0

      //Eigen::VectorXd diff2 = dp2 - apply_index(dp1,match1);
      //double cost_bw = diff2.squaredNorm()/dp2.rows();

      Eigen::MatrixXd dX1(avg_grad1.rows(),avg_grad1.cols());
      dX1<< avg_grad1.col(0).array() * scale1.array(),
            avg_grad1.col(1).array() * scale1.array(),
            avg_grad1.col(2).array() * scale1.array(); //# bsxfun(@times, (.5*dC1s + .5*dC2s(corr12d,:)), scale);         % symmetric
      //Eigen::MatrixXd dX1 =(avg_grad1.array()*avg_grad1.array())*-1.0/ (avg_grad1.array()*diff1.array()+lambda);
      // simple case (no damping)

      // sparse matrix version
      // construct jacobian (sparse matrix)
      // using T = Eigen::Triplet<double>;
      // Eigen::SparseMatrix<double> J(avg_grad1.rows(), avg_grad1.rows()*avg_grad1.cols());
      // std::vector<T > Jij;
      // Jij.reserve( avg_grad1.rows()*avg_grad1.cols() ) ;

      // // implies "col-major" solution here
      // for(int k = 0; k<avg_grad1.cols(); ++k)
      // {
      //   for(int j = 0; j<avg_grad1.rows(); ++j)
      //   {
      //       Jij.push_back(T(j, j+k*dp1.rows(), avg_grad1(j,k)));
      //   }
      // }
      // J.setFromTriplets(Jij.begin(), Jij.end());

      // Eigen::SparseMatrix<double> JtJ = J.transpose()*J;
      // Eigen::SparseMatrix<double> Lhs = JtJ + lambda*Eigen::SparseMatrix<double>( JtJ.diagonal().asDiagonal() );
      // Eigen::VectorXd Rhs             = J.transpose() * diff1;

      // if(i==0) // initializae preconditioner once
      // {
      //   solver.analyzePattern(Lhs);
      // }

      // // solver.preconditioner().setDroptol(0.01);
      // solver.factorize(Lhs);
      // Eigen::VectorXd solution;

      // if(solver.info()!= Eigen::Success) {
      //     std::cerr<<"Solving failed"<<std::endl;
      //     return 1; 
      //     //TODO: handle this differently!
      // } else {
      //     solution = solver.solve(Rhs);
      // }
      // //std::cout << "  #inner iterations: " << solver.iterations() << ", estimated error: " << solver.error() << std::endl;

      // // trick to access data as Matrix
      // Eigen::Map<const Eigen::MatrixXd > 
      //   _dX1(solution.data(), dp1.rows(), 3); 

      // Eigen::MatrixXd dX1 = _dX1;

      // TODO:Smooth update
      for(auto j=0; j<smooth_update; ++j)      
          dX1 = (Lsource * dX1).eval();

      // finally : update
      sph1     = sph1 + dX1 * demons_step;

      // re-project on the surface here?
      sph1.rowwise().normalize();

      std::cout<<i<<"\t:" << cost_fw<<":" << dX1.rowwise().norm().mean() << "\t"<<std::endl; //solver.error()<< 
    }

    Eigen::MatrixXd DO(D1.rows(),5);
    //HACK: don't preserve any other user data here
    xyz_to_sph(sph1,PT1);

    DO<<PT1, C1s, Lsource*Lsource*Lsource * C1s,
      (sph1.array()*sph1_orig.array()).rowwise().sum().acos()*180.0/M_PI ;

    std::vector<std::string> headerO({"psi","the","dp","sdp","da"});
    
    igl::writePLY(par["output"].as<std::string>(), V1, F1, E1, N1, UV1, DO, headerO );
  } else {
    std::cerr << options.help({"", "Group"}) << std::endl;
    return 1;
  }
  return 0;
}
