
#include <ct/rbd/rbd.h>
#include "../../exampleDir.h"

#include <ct/models/InvertedPendulum/InvertedPendulum.h>

#include "../MPCSimulatorROS.h"

int main(int argc, char* argv[])
{
    ros::init(argc, argv, "InvertedPendulum_mpc");
    ros::NodeHandle nh("~");

    const bool verbose = true;

    using namespace ct::rbd;

    const size_t njoints = ct::rbd::InvertedPendulum::Kinematics::NJOINTS;
    const size_t actuator_state_dim = 1;

    using RobotState_t = ct::rbd::FixBaseRobotState<njoints, actuator_state_dim>;
    static const size_t state_dim = RobotState_t::NSTATE;
    static const size_t control_dim = njoints;

    using IPDynamics = ct::rbd::InvertedPendulum::tpl::Dynamics<double>;
    using IPSystem = ct::rbd::FixBaseFDSystem<IPDynamics, actuator_state_dim, false>;
    using LinearSystem = ct::core::LinearSystem<state_dim, control_dim>;
    using InvertedPendulumNLOC = FixBaseNLOC<IPSystem>;

    try
    {
        std::string workingDirectory = ct::ros::exampleDir + "/mpc/InvertedPendulum";

        std::string configFile = workingDirectory + "/solver.info";
        std::string costFunctionFile = workingDirectory + "/cost.info";

        const double k_spring = 160;
        const double gear_ratio = 50;

        std::shared_ptr<ct::rbd::SEADynamicsFirstOrder<njoints>> actuatorDynamics(
            new ct::rbd::SEADynamicsFirstOrder<njoints>(k_spring, gear_ratio));
        std::shared_ptr<IPSystem> ipSystem(new IPSystem(actuatorDynamics));

        // NLOC settings
        ct::optcon::NLOptConSettings nloc_settings;
        nloc_settings.load(configFile, verbose, "nloc");

        std::shared_ptr<ct::optcon::TermQuadratic<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>> termQuadInterm(
            new ct::optcon::TermQuadratic<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>);
        termQuadInterm->loadConfigFile(costFunctionFile, "term0", verbose);

        std::shared_ptr<ct::optcon::TermQuadratic<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>> termQuadFinal(
            new ct::optcon::TermQuadratic<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>);
        termQuadFinal->loadConfigFile(costFunctionFile, "term1", verbose);

        std::shared_ptr<ct::optcon::CostFunctionAnalytical<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>> newCost(
            new ct::optcon::CostFunctionAnalytical<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>);
        size_t intTermID = newCost->addIntermediateTerm(termQuadInterm);
        size_t finalTermID = newCost->addFinalTerm(termQuadFinal);

        ct::core::Time timeHorizon;
        InvertedPendulumNLOC::FeedbackArray::value_type fbD;
        RobotState_t x0;
        RobotState_t xf;

        ct::core::loadScalar(configFile, "timeHorizon", timeHorizon);
        ct::core::loadMatrix(costFunctionFile, "K_init", fbD);
        RobotState_t::state_vector_t xftemp, x0temp;
        ct::core::loadMatrix(costFunctionFile, "x_0", x0temp);
        ct::core::loadMatrix(costFunctionFile, "term1.weights.x_des", xftemp);
        x0.fromStateVector(x0temp);
        xf.fromStateVector(xftemp);

        std::shared_ptr<LinearSystem> linSystem = nullptr;

        ct::optcon::ContinuousOptConProblem<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM> optConProblem(
            timeHorizon, x0.toStateVector(), ipSystem, newCost, linSystem);

        InvertedPendulumNLOC nloc_solver(newCost, nloc_settings, ipSystem, verbose, linSystem);

        int K = nloc_solver.getSettings().computeK(timeHorizon);

        InvertedPendulumNLOC::StateVectorArray stateRefTraj(K + 1, x0.toStateVector());
        InvertedPendulumNLOC::FeedbackArray fbTrajectory(K, -fbD);
        InvertedPendulumNLOC::ControlVectorArray ffTrajectory(K, InvertedPendulumNLOC::ControlVector::Zero());

        int initType = 0;
        ct::core::loadScalar(configFile, "initType", initType);

        switch (initType)
        {
            case 0:  // steady state
            {
                ct::core::ControlVector<IPSystem::CONTROL_DIM> uff_ref;
                nloc_solver.initializeSteadyPose(x0, timeHorizon, K, uff_ref, -fbD);

                std::vector<
                    std::shared_ptr<ct::optcon::CostFunctionQuadratic<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>>>&
                    inst1 = nloc_solver.getSolver()->getCostFunctionInstances();

                for (size_t i = 0; i < inst1.size(); i++)
                {
                    inst1[i]->getIntermediateTermById(intTermID)->updateReferenceControl(uff_ref);
                }
                break;
            }
            case 1:  // linear interpolation
            {
                nloc_solver.initializeDirectInterpolation(x0, xf, timeHorizon, K, -fbD);
                break;
            }
            default:
            {
                throw std::runtime_error("illegal init type");
                break;
            }
        }

        nloc_solver.solve();
        ct::core::StateFeedbackController<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM> initialSolution =
            nloc_solver.getSolution();
        InvertedPendulumNLOC::StateVectorArray x_nloc = initialSolution.x_ref();


        ct::optcon::NLOptConSettings ilqr_settings_mpc(nloc_solver.getSettings());
        ilqr_settings_mpc.max_iterations = 1;
        ilqr_settings_mpc.printSummary = false;

        ct::optcon::mpc_settings mpc_settings;
        mpc_settings.stateForwardIntegration_ = false;
        mpc_settings.postTruncation_ = false;
        mpc_settings.measureDelay_ = false;
        mpc_settings.delayMeasurementMultiplier_ = 1.0;
        mpc_settings.mpc_mode = ct::optcon::MPC_MODE::CONSTANT_RECEDING_HORIZON;
        mpc_settings.coldStart_ = false;

        ct::optcon::MPC<ct::optcon::NLOptConSolver<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>> ilqr_mpc(
            optConProblem, ilqr_settings_mpc, mpc_settings);
        ilqr_mpc.setInitialGuess(initialSolution);
        ipSystem->setController(std::shared_ptr<ct::core::StateFeedbackController<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>> (
        		new ct::core::StateFeedbackController<IPSystem::STATE_DIM, IPSystem::CONTROL_DIM>(initialSolution)));


        std::shared_ptr<ct::ros::RBDStatePublisher> statePublisher( new ct::ros::RBDStatePublisher(
        		ct::models::InvertedPendulum::urdfJointNames(), "/ip/InvertedPendulumBase", "/world"));
        statePublisher->advertise(nh, "/current_joint_states", 10);

        ct::core::Time sim_dt;
        ct::core::loadScalar(configFile, "nloc.dt", sim_dt);


        MPCSimulatorROS<IPSystem> mpc_sim(statePublisher, initialSolution, sim_dt, sim_dt, x0, ipSystem, ilqr_mpc);

        std::cout << "waiting 5 second for begin" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));

        std::cout << "simulating 10 seconds" << std::endl;
        mpc_sim.simulate(10.0);
        mpc_sim.finish();

        ilqr_mpc.printMpcSummary();

    } catch (std::runtime_error& e)
    {
        std::cout << "Exception caught: " << e.what() << std::endl;
    }
}
