/**********************************************************************************************************************
This file is part of the Control Toolbox (https://github.com/ethz-adrl/control-toolbox), copyright by ETH Zurich.
Authors:  Michael Neunert, Markus Giftthaler, Markus Stäuble, Diego Pardo, Farbod Farshidian
Licensed under BSD-2 license (see LICENSE file in main directory)
**********************************************************************************************************************/

//#define MATLAB
//#define MATLAB_FULL_LOG
//#define DEBUG_PRINT_MP

#include <ct/rbd/rbd.h>
#include <ct/ros/ros.h>

#include <ct/models/Franka/Franka.h>
#include <ct/models/Franka/FrankaInverseKinematics.h>

using namespace ct::rbd;

const size_t njoints = ct::rbd::Franka::Kinematics::NJOINTS;
using RobotState_t = FixBaseRobotState<njoints>;

const size_t state_dim = RobotState_t::NSTATE;
const size_t control_dim = njoints;

using FrankaDynamics = ct::rbd::Franka::Dynamics;
using FrankaSystem = ct::rbd::FixBaseFDSystem<FrankaDynamics>;
using LinearSystem = ct::core::LinearSystem<state_dim, control_dim>;
using FrankaLinearCodegen = ct::models::Franka::FrankaLinearizedForward;

RobotState_t x0;  // init state


int main(int argc, char* argv[])
{
    ros::init(argc, argv, "franka_nloc_taskspace");
    ros::NodeHandle nh("~");

    ROS_INFO("Set up visualizers");
    std::string frameId = "table";
    ct::ros::RBDStatePublisher statePublisher(ct::models::Franka::urdfJointNames(), "panda_link0", frameId);
    statePublisher.advertise(nh, "/joint_states", 10);

    std::shared_ptr<ct::ros::PoseVisualizer> targetPoseVisualizer(
        new ct::ros::PoseVisualizer("franka/table", "target_pose"));
    std::shared_ptr<ct::ros::PoseVisualizer> currentPoseVisualizer(
        new ct::ros::PoseVisualizer("franka/table", "current_pose"));

    ct::ros::VisNode<geometry_msgs::PoseStamped> visNode_poseDes(nh, std::string("ee_ref_pose_visualizer"));
    ct::ros::VisNode<geometry_msgs::PoseStamped> visNode_poseCurr(nh, std::string("ee_current_pose_visualizer"));

    visNode_poseDes.addVisualizer(targetPoseVisualizer);
    visNode_poseCurr.addVisualizer(currentPoseVisualizer);

    ROS_INFO("Loading NLOC config files");

    std::string workingDirectory;
    if (!nh.getParam("workingDirectory", workingDirectory))
        std::cout << "Working directory parameter 'workingDirectory' not set" << std::endl;

    std::cout << "Working directory is: " << workingDirectory << std::endl;
    std::string configFile = workingDirectory + "/solver.info";
    std::string costFunctionFile = workingDirectory + "/cost.info";

    ROS_INFO("Setting up system");
    std::shared_ptr<FrankaSystem> system(new FrankaSystem);
    std::shared_ptr<LinearSystem> linSystem(new FrankaLinearCodegen);

    ROS_INFO("Initializing NLOC");

    // load x0
    RobotState_t::state_vector_t x0_load;
    ct::core::loadMatrix(costFunctionFile, "x_0", x0_load);
    x0.fromStateVector(x0_load);

    // load init feedback
    FixBaseNLOC<FrankaSystem>::FeedbackArray::value_type fbD;
    ct::core::loadMatrix(costFunctionFile, "K_init", fbD);

    // NLOC settings
    ct::optcon::NLOptConSettings nloc_settings;
    nloc_settings.load(configFile, true, "nloc");

    // Setup Cost function
    std::shared_ptr<ct::optcon::CostFunctionAnalytical<state_dim, control_dim>> costFun(
        new ct::optcon::CostFunctionAnalytical<state_dim, control_dim>());

    ROS_INFO("Setting up task-space cost term");
    using FrankaKinematics_t = Franka::tpl::Kinematics<double>;


    // task space cost term
    using TermTaskspacePose = ct::rbd::TermTaskspaceGeometricJacobian<FrankaKinematics_t, state_dim, control_dim>;
    std::shared_ptr<TermTaskspacePose> termTaskSpace_intermediate(
        new TermTaskspacePose(costFunctionFile, "termTaskSpace_intermediate", true));
    costFun->addIntermediateTerm(termTaskSpace_intermediate, true);

    std::shared_ptr<TermTaskspacePose> termTaskSpace_final(
        new TermTaskspacePose(costFunctionFile, "termTaskSpace_final", true));
    size_t task_space_term_id = costFun->addFinalTerm(termTaskSpace_final, true);


    ROS_INFO("Solving Inverse Kinematics for Initial Guess");
    ct::rbd::RigidBodyPose ee_pose_des = termTaskSpace_final->getReferencePose();

    ROS_INFO("Setting up joint-space cost terms");
    using TermQuadratic = ct::optcon::TermQuadratic<state_dim, control_dim>;
    std::shared_ptr<TermQuadratic> termQuadInterm(new TermQuadratic(costFunctionFile, "term_quad_intermediate"));
    std::shared_ptr<TermQuadratic> termQuadFinal(new TermQuadratic(costFunctionFile, "term_quad_final"));

    size_t intTermID = costFun->addIntermediateTerm(termQuadInterm);
    size_t term_quad_final_id = costFun->addFinalTerm(termQuadFinal);
    costFun->initialize();


    /* STEP 1-D: set up the general constraints */
    // constraint terms
    ROS_INFO("Setting up joint-space constraints");

    // create constraint container
    std::shared_ptr<ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>> inputBoxConstraints(
        new ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>());

    std::shared_ptr<ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>> stateBoxConstraints(
        new ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>());

    // input constraint bounds
    ct::core::ControlVector<control_dim> u_lb = -50 * ct::core::ControlVector<control_dim>::Ones();
    ct::core::ControlVector<control_dim> u_ub = 50 * ct::core::ControlVector<control_dim>::Ones();
    // input constraint terms
    std::shared_ptr<ct::optcon::ControlInputConstraint<state_dim, control_dim>> controlConstraint(
        new ct::optcon::ControlInputConstraint<state_dim, control_dim>(u_lb, u_ub));
    controlConstraint->setName("ControlInputConstraint");
    // add and initialize constraint terms
    inputBoxConstraints->addIntermediateConstraint(controlConstraint, true);
    inputBoxConstraints->initialize();


    // state constraint bounds
    ct::core::StateVector<state_dim> x_lb, x_ub;
    x_lb.head<njoints>() = -3.14 * Eigen::Matrix<double, njoints, 1>::Ones();  // lower bound on position
    x_ub.head<njoints>() = 3.14 * Eigen::Matrix<double, njoints, 1>::Ones();   // upper bound on position
    x_lb.tail<njoints>() = -2 * Eigen::Matrix<double, njoints, 1>::Ones();     // lower bound on velocity
    x_ub.tail<njoints>() = 2 * Eigen::Matrix<double, njoints, 1>::Ones();      // upper bound on velocity
    // state constraint terms
    std::shared_ptr<ct::optcon::StateConstraint<state_dim, control_dim>> stateConstraint(
        new ct::optcon::StateConstraint<state_dim, control_dim>(x_lb, x_ub));
    stateConstraint->setName("StateConstraint");
    // add and initialize constraint terms
    stateBoxConstraints->addIntermediateConstraint(stateConstraint, true);
    stateBoxConstraints->addTerminalConstraint(stateConstraint, true);
    stateBoxConstraints->initialize();


    ROS_INFO("Creating solvers now");
    FixBaseNLOC<FrankaSystem> nloc(
        costFun, inputBoxConstraints, stateBoxConstraints, nullptr, nloc_settings, system, true, linSystem);
    ROS_INFO("Finished creating solvers.");

    ct::core::Time timeHorizon;
    ct::core::loadScalar(configFile, "timeHorizon", timeHorizon);

    int K = nloc.getSettings().computeK(timeHorizon);


    // init
    ct::core::ControlVector<FrankaSystem::CONTROL_DIM> uff_ref;
    nloc.initializeSteadyPose(x0, timeHorizon, K, uff_ref, -fbD);

    std::vector<std::shared_ptr<ct::optcon::CostFunctionQuadratic<state_dim, control_dim>>>& inst1 =
        nloc.getSolver()->getCostFunctionInstances();

    ROS_INFO("Starting to update cost function instances");
    for (size_t i = 0; i < inst1.size(); i++)
    {
        inst1[i]->getIntermediateTermById(intTermID)->updateReferenceControl(uff_ref);
        inst1[i]->getIntermediateTermById(intTermID)->updateReferenceState(x0.toStateVector());
        inst1[i]->getFinalTermById(term_quad_final_id)->updateReferenceState(x0.toStateVector());
    }


    ROS_INFO("Solving problem ...");

    nloc.solve();
    typename FixBaseNLOC<FrankaSystem>::StateVectorArray x_solution = nloc.getSolution().x_ref();
    typename FixBaseNLOC<FrankaSystem>::ControlVectorArray u_solution = nloc.getSolution().uff();


    do
    {
        std::cout << '\n' << "Press a key to continue...";
    } while (std::cin.get() != '\n');

    while (ros::ok())
    {
        ROS_INFO("Visualizing ...");

        ros::Rate publishRate(1. / nloc.getSettings().dt);

        for (size_t i = 0; i < x_solution.size(); i++)
        {
            targetPoseVisualizer->setPose(ee_pose_des);
            visNode_poseDes.visualize();

            ct::rbd::Franka::Kinematics kinematics;
            size_t eeInd = 0;
            ct::rbd::RigidBodyPose eePoseCurr =
                kinematics.getEEPoseInBase(eeInd, x_solution[i].template cast<double>().head<njoints>());
            currentPoseVisualizer->setPose(eePoseCurr);
            visNode_poseCurr.visualize();

            RBDState<njoints> state;
            state.setZero();
            state.jointPositions() = x_solution[i].template cast<double>().head<njoints>();
            state.jointVelocities() = x_solution[i].template cast<double>().tail<njoints>();

            statePublisher.publishState(state);
            publishRate.sleep();
        }

        ros::Duration(1.0).sleep();
    }
}
