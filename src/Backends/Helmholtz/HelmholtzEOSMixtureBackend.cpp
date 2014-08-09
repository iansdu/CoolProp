/*
 * AbstractBackend.cpp
 *
 *  Created on: 20 Dec 2013
 *      Author: jowr
 */

#include <memory>

#if defined(_MSC_VER)
#define _CRTDBG_MAP_ALLOC
#define _CRT_SECURE_NO_WARNINGS
#include <crtdbg.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#include <string>
//#include "CoolProp.h"

#include "HelmholtzEOSMixtureBackend.h"
#include "Fluids/FluidLibrary.h"
#include "Solvers.h"
#include "MatrixMath.h"
#include "VLERoutines.h"
#include "FlashRoutines.h"
#include "TransportRoutines.h"

namespace CoolProp {

HelmholtzEOSMixtureBackend::HelmholtzEOSMixtureBackend(std::vector<std::string> &component_names, bool generate_SatL_and_SatV) {
    std::vector<CoolPropFluid*> components;
    components.resize(component_names.size());

    for (unsigned int i = 0; i < components.size(); ++i)
    {
        components[i] = &(get_library().get(component_names[i]));
    }

    // Set the components and associated flags
    set_components(components, generate_SatL_and_SatV);
    
    // Set the phase to default unknown value
    _phase = iphase_unknown;
}
HelmholtzEOSMixtureBackend::HelmholtzEOSMixtureBackend(std::vector<CoolPropFluid*> components, bool generate_SatL_and_SatV) {

    // Set the components and associated flags
    set_components(components, generate_SatL_and_SatV);
    
    // Set the phase to default unknown value
    _phase = iphase_unknown;
}
void HelmholtzEOSMixtureBackend::set_components(std::vector<CoolPropFluid*> components, bool generate_SatL_and_SatV) {

    // Copy the components
    this->components = components;
    this->N = components.size();

    if (components.size() == 1){
        is_pure_or_pseudopure = true;
        mole_fractions = std::vector<long double>(1, 1);
    }
    else{
        is_pure_or_pseudopure = false;
    }

    // Set the excess Helmholtz energy if a mixture
    if (!is_pure_or_pseudopure)
    {
        // Set the reducing model
        set_reducing_function();
        set_excess_term();
    }

    imposed_phase_index = -1;

    // Top-level class can hold copies of the base saturation classes,
    // saturation classes cannot hold copies of the saturation classes
    if (generate_SatL_and_SatV)
    {
        SatL.reset(new HelmholtzEOSMixtureBackend(components, false));
        SatL->specify_phase(iphase_liquid);
        SatV.reset(new HelmholtzEOSMixtureBackend(components, false));
        SatV->specify_phase(iphase_gas);
    }
}
void HelmholtzEOSMixtureBackend::set_mole_fractions(const std::vector<long double> &mole_fractions)
{
    if (mole_fractions.size() != N)
    {
        throw ValueError(format("size of mole fraction vector [%d] does not equal that of component vector [%d]",mole_fractions.size(), N));
    }
    // Copy values without reallocating memory
    this->resize(N);
    std::copy( mole_fractions.begin(), mole_fractions.end(), this->mole_fractions.begin() );
    // Resize the vectors for the liquid and vapor,  but only if they are in use
    if (this->SatL.get() != NULL){
        this->SatL->resize(N);
    }
    if (this->SatV.get() != NULL){
        this->SatV->resize(N);
    }
};
void HelmholtzEOSMixtureBackend::resize(unsigned int N)
{
    this->mole_fractions.resize(N);
    this->K.resize(N);
    this->lnK.resize(N);
}
void HelmholtzEOSMixtureBackend::set_reducing_function()
{
    Reducing.set(ReducingFunction::factory(components));
}
void HelmholtzEOSMixtureBackend::set_excess_term()
{
    Excess.construct(components);
}
void HelmholtzEOSMixtureBackend::update_states(void)
{
    CoolPropFluid &component = *(components[0]);
    EquationOfState &EOS = component.EOSVector[0];
    long double rho, T;
    // Clear the state class
    clear();
    // Calculate the new enthalpy and entropy values
    update(DmolarT_INPUTS, EOS.hs_anchor.rhomolar, EOS.hs_anchor.T);
    EOS.hs_anchor.hmolar = hmolar();
    EOS.hs_anchor.smolar = smolar();
    // Clear again just to be sure
    clear();
}
long double HelmholtzEOSMixtureBackend::calc_gas_constant(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i)
    {
        summer += mole_fractions[i]*components[i]->gas_constant();
    }
    return summer;
}
long double HelmholtzEOSMixtureBackend::calc_molar_mass(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i)
    {
        summer += mole_fractions[i]*components[i]->molar_mass();
    }
    return summer;
}

long double HelmholtzEOSMixtureBackend::calc_melting_line(int param, int given, long double value)
{
    if (is_pure_or_pseudopure)
    {
        return components[0]->ancillaries.melting_line.evaluate(param, given, value);
    }
    else
    {
        throw NotImplementedError(format("calc_melting_line not implemented for mixtures"));
    }
}
long double HelmholtzEOSMixtureBackend::calc_viscosity_dilute(void)
{
    if (is_pure_or_pseudopure)
    {
        long double eta_dilute;
        switch(components[0]->transport.viscosity_dilute.type)
        {
        case ViscosityDiluteVariables::VISCOSITY_DILUTE_KINETIC_THEORY:
            eta_dilute = TransportRoutines::viscosity_dilute_kinetic_theory(*this); break;
        case ViscosityDiluteVariables::VISCOSITY_DILUTE_COLLISION_INTEGRAL:
            eta_dilute = TransportRoutines::viscosity_dilute_collision_integral(*this); break;
        case ViscosityDiluteVariables::VISCOSITY_DILUTE_POWERS_OF_T:
            eta_dilute = TransportRoutines::viscosity_dilute_powers_of_T(*this); break;
        case ViscosityDiluteVariables::VISCOSITY_DILUTE_COLLISION_INTEGRAL_POWERS_OF_TSTAR:
            eta_dilute = TransportRoutines::viscosity_dilute_collision_integral_powers_of_T(*this); break;
        case ViscosityDiluteVariables::VISCOSITY_DILUTE_ETHANE:
            eta_dilute = TransportRoutines::viscosity_dilute_ethane(*this); break;
        default:
            throw ValueError(format("dilute viscosity type [%d] is invalid for fluid %s", components[0]->transport.viscosity_dilute.type, name().c_str()));
        }
        return eta_dilute;
    }
    else
    {
        throw NotImplementedError(format("dilute viscosity not implemented for mixtures"));
    }

}
long double HelmholtzEOSMixtureBackend::calc_viscosity_background()
{
    long double eta_dilute = calc_viscosity_dilute();
    return calc_viscosity_background(eta_dilute);
}
long double HelmholtzEOSMixtureBackend::calc_viscosity_background(long double eta_dilute)
{
    // Residual part
    long double B_eta_initial = TransportRoutines::viscosity_initial_density_dependence_Rainwater_Friend(*this);
    long double rho = rhomolar();
    long double initial_part = eta_dilute*B_eta_initial*rho;

    // Higher order terms
    long double delta_eta_h;
    switch(components[0]->transport.viscosity_higher_order.type)
    {
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_BATSCHINKI_HILDEBRAND:
        delta_eta_h = TransportRoutines::viscosity_higher_order_modified_Batschinski_Hildebrand(*this); break;
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_FRICTION_THEORY:
        delta_eta_h = TransportRoutines::viscosity_higher_order_friction_theory(*this); break;
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_HYDROGEN:
        delta_eta_h = TransportRoutines::viscosity_hydrogen_higher_order_hardcoded(*this); break;
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_HEXANE:
        delta_eta_h = TransportRoutines::viscosity_hexane_higher_order_hardcoded(*this); break;
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_HEPTANE:
        delta_eta_h = TransportRoutines::viscosity_heptane_higher_order_hardcoded(*this); break;
    case ViscosityHigherOrderVariables::VISCOSITY_HIGHER_ORDER_ETHANE:
        delta_eta_h = TransportRoutines::viscosity_ethane_higher_order_hardcoded(*this); break;
    default:
        throw ValueError(format("higher order viscosity type [%d] is invalid for fluid %s", components[0]->transport.viscosity_dilute.type, name().c_str()));
    }

    long double eta_residual = initial_part + delta_eta_h;

    return eta_residual;
}

long double HelmholtzEOSMixtureBackend::calc_viscosity(void)
{
    if (is_pure_or_pseudopure)
    {
        // Get a reference for code cleanness
        CoolPropFluid &component = *(components[0]);

        // Check if using ECS
        if (component.transport.viscosity_using_ECS)
        {
            // Get reference fluid name
            std::string fluid_name  = component.transport.viscosity_ecs.reference_fluid;
            std::vector<std::string> names(1, fluid_name);
            // Get a managed pointer to the reference fluid for ECS
            shared_ptr<HelmholtzEOSMixtureBackend> ref_fluid(new HelmholtzEOSMixtureBackend(names));
            // Get the viscosity using ECS
            return TransportRoutines::viscosity_ECS(*this, *(ref_fluid.get()));
        }

        if (component.transport.hardcoded_viscosity != CoolProp::TransportPropertyData::VISCOSITY_NOT_HARDCODED)
        {
            switch(component.transport.hardcoded_viscosity)
            {
            case CoolProp::TransportPropertyData::VISCOSITY_HARDCODED_WATER:
                return TransportRoutines::viscosity_water_hardcoded(*this);
            case CoolProp::TransportPropertyData::VISCOSITY_HARDCODED_HELIUM:
                return TransportRoutines::viscosity_helium_hardcoded(*this);
            case CoolProp::TransportPropertyData::VISCOSITY_HARDCODED_R23:
                return TransportRoutines::viscosity_R23_hardcoded(*this);
            default:
                throw ValueError(format("hardcoded viscosity type [%d] is invalid for fluid %s", component.transport.hardcoded_viscosity, name().c_str()));
            }
        }
        // Dilute part
        long double eta_dilute = calc_viscosity_dilute();

        // Background viscosity given by the sum of the initial density dependence and higher order terms
        long double eta_back = calc_viscosity_background(eta_dilute);

        // Critical part (no fluids have critical enhancement for viscosity currently)
        long double eta_critical = 0;

        return eta_dilute + eta_back + eta_critical;
    }
    else
    {
        throw NotImplementedError(format("viscosity not implemented for mixtures"));
    }
}
long double HelmholtzEOSMixtureBackend::calc_conductivity_background(void)
{
    // Residual part
    long double lambda_residual = _HUGE;
    switch(components[0]->transport.conductivity_residual.type)
    {
    case ConductivityResidualVariables::CONDUCTIVITY_RESIDUAL_POLYNOMIAL:
        lambda_residual = TransportRoutines::conductivity_residual_polynomial(*this); break;
    case ConductivityResidualVariables::CONDUCTIVITY_RESIDUAL_POLYNOMIAL_AND_EXPONENTIAL:
        lambda_residual = TransportRoutines::conductivity_residual_polynomial_and_exponential(*this); break;
    default:
        throw ValueError(format("residual conductivity type [%d] is invalid for fluid %s", components[0]->transport.conductivity_residual.type, name().c_str()));
    }
    return lambda_residual;
}
long double HelmholtzEOSMixtureBackend::calc_conductivity(void)
{
    if (is_pure_or_pseudopure)
    {
        // Get a reference for code cleanness
        CoolPropFluid &component = *(components[0]);

        // Check if using ECS
        if (component.transport.conductivity_using_ECS)
        {
            // Get reference fluid name
            std::string fluid_name  = component.transport.conductivity_ecs.reference_fluid;
            std::vector<std::string> name(1, fluid_name);
            // Get a managed pointer to the reference fluid for ECS
            shared_ptr<HelmholtzEOSMixtureBackend> ref_fluid(new HelmholtzEOSMixtureBackend(name,false));
            // Get the viscosity using ECS
            return TransportRoutines::conductivity_ECS(*this, *(ref_fluid.get()));
        }

        if (component.transport.hardcoded_conductivity != CoolProp::TransportPropertyData::CONDUCTIVITY_NOT_HARDCODED)
        {
            switch(component.transport.hardcoded_conductivity)
            {
            case CoolProp::TransportPropertyData::CONDUCTIVITY_HARDCODED_WATER:
                return TransportRoutines::conductivity_hardcoded_water(*this);
            case CoolProp::TransportPropertyData::CONDUCTIVITY_HARDCODED_R23:
                return TransportRoutines::conductivity_hardcoded_R23(*this);
            case CoolProp::TransportPropertyData::CONDUCTIVITY_HARDCODED_HELIUM:
                return TransportRoutines::conductivity_hardcoded_helium(*this);
            default:
                throw ValueError(format("hardcoded viscosity type [%d] is invalid for fluid %s", components[0]->transport.hardcoded_conductivity, name().c_str()));
            }
        }

        // Dilute part
        long double lambda_dilute = _HUGE;
        switch(component.transport.conductivity_dilute.type)
        {
        case ConductivityDiluteVariables::CONDUCTIVITY_DILUTE_RATIO_POLYNOMIALS:
            lambda_dilute = TransportRoutines::conductivity_dilute_ratio_polynomials(*this); break;
        case ConductivityDiluteVariables::CONDUCTIVITY_DILUTE_ETA0_AND_POLY:
            lambda_dilute = TransportRoutines::conductivity_dilute_eta0_and_poly(*this); break;
        case ConductivityDiluteVariables::CONDUCTIVITY_DILUTE_CO2:
            lambda_dilute = TransportRoutines::conductivity_dilute_hardcoded_CO2(*this); break;
        case ConductivityDiluteVariables::CONDUCTIVITY_DILUTE_ETHANE:
            lambda_dilute = TransportRoutines::conductivity_dilute_hardcoded_ethane(*this); break;
        case ConductivityDiluteVariables::CONDUCTIVITY_DILUTE_NONE:
            lambda_dilute = 0.0; break;
        default:
            throw ValueError(format("dilute conductivity type [%d] is invalid for fluid %s", components[0]->transport.conductivity_dilute.type, name().c_str()));
        }

        long double lambda_residual = calc_conductivity_background();

        // Critical part
        long double lambda_critical = _HUGE;
        switch(component.transport.conductivity_critical.type)
        {
        case ConductivityCriticalVariables::CONDUCTIVITY_CRITICAL_SIMPLIFIED_OLCHOWY_SENGERS:
            lambda_critical = TransportRoutines::conductivity_critical_simplified_Olchowy_Sengers(*this); break;
        case ConductivityCriticalVariables::CONDUCTIVITY_CRITICAL_R123:
            lambda_critical = TransportRoutines::conductivity_critical_hardcoded_R123(*this); break;
        case ConductivityCriticalVariables::CONDUCTIVITY_CRITICAL_AMMONIA:
            lambda_critical = TransportRoutines::conductivity_critical_hardcoded_ammonia(*this); break;
        case ConductivityCriticalVariables::CONDUCTIVITY_CRITICAL_NONE:
            lambda_critical = 0.0; break;
        case ConductivityCriticalVariables::CONDUCTIVITY_CRITICAL_CARBONDIOXIDE_SCALABRIN_JPCRD_2006:
            lambda_critical = TransportRoutines::conductivity_critical_hardcoded_CO2_ScalabrinJPCRD2006(*this); break;
        default:
            throw ValueError(format("critical conductivity type [%d] is invalid for fluid %s", components[0]->transport.viscosity_dilute.type, name().c_str()));
        }

        return lambda_dilute + lambda_residual + lambda_critical;
    }
    else
    {
        throw NotImplementedError(format("viscosity not implemented for mixtures"));
    }
}
long double HelmholtzEOSMixtureBackend::calc_Ttriple(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i){
        summer += mole_fractions[i]*components[i]->pEOS->Ttriple;
    }
    return summer;
}
std::string HelmholtzEOSMixtureBackend::calc_name(void)
{
    if (components.size() != 1){
        throw ValueError(format("calc_name is only valid for pure and pseudo-pure fluids, %d components", components.size()));
    }
    else{
        return components[0]->name;
    }
}
long double HelmholtzEOSMixtureBackend::calc_T_critical(void)
{
    if (components.size() != 1){
        throw ValueError(format("For now, calc_T_critical is only valid for pure and pseudo-pure fluids, %d components", components.size()));
    }
    else{
        return components[0]->crit.T;
    }
}
long double HelmholtzEOSMixtureBackend::calc_p_critical(void)
{
    if (components.size() != 1){
        throw ValueError(format("For now, calc_p_critical is only valid for pure and pseudo-pure fluids, %d components", components.size()));
    }
    else{
        return components[0]->crit.p;
    }
}
long double HelmholtzEOSMixtureBackend::calc_rhomolar_critical(void)
{
    if (components.size() != 1){
        throw ValueError(format("For now, calc_rhomolar_critical is only valid for pure and pseudo-pure fluids, %d components", components.size()));
    }
    else{
        return components[0]->crit.rhomolar;
    }
}
long double HelmholtzEOSMixtureBackend::calc_pmax_sat(void)
{
	if (is_pure_or_pseudopure)
	{
		if (components[0]->pEOS->pseudo_pure)
		{
			return components[0]->pEOS->max_sat_p.p;
		}
		else{
			return p_critical();
		}
	}
	else{
		throw ValueError("calc_pmax_sat not yet defined for mixtures");
	}
}
long double HelmholtzEOSMixtureBackend::calc_Tmax_sat(void)
{
	if (is_pure_or_pseudopure)
	{
		if (components[0]->pEOS->pseudo_pure)
		{
			return components[0]->pEOS->max_sat_T.T;
		}
		else{
			return T_critical();
		}
	}
	else{
		throw ValueError("calc_Tmax_sat not yet defined for mixtures");
	}
}

void HelmholtzEOSMixtureBackend::calc_Tmin_sat(long double &Tmin_satL, long double &Tmin_satV)
{
	if (is_pure_or_pseudopure)
	{
		Tmin_satL = components[0]->pEOS->sat_min_liquid.T;
		Tmin_satV = components[0]->pEOS->sat_min_vapor.T;
		return;
	}
	else{
		throw ValueError("calc_Tmin_sat not yet defined for mixtures");
	}
}

void HelmholtzEOSMixtureBackend::calc_pmin_sat(long double &pmin_satL, long double &pmin_satV)
{
	if (is_pure_or_pseudopure)
	{
		pmin_satL = components[0]->pEOS->sat_min_liquid.p;
		pmin_satV = components[0]->pEOS->sat_min_vapor.p;
		return;
	}
	else{
		throw ValueError("calc_pmin_sat not yet defined for mixtures");
	}
}

// Minimum allowed saturation temperature the maximum of the saturation temperatures of liquid and vapor
		// For pure fluids, both values are the same, for pseudo-pure they are probably the same, for mixtures they are definitely not the same

long double HelmholtzEOSMixtureBackend::calc_Tmax(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i)
    {
        summer += mole_fractions[i]*components[i]->pEOS->limits.Tmax;
    }
    return summer;
}
long double HelmholtzEOSMixtureBackend::calc_Tmin(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i)
    {
        summer += mole_fractions[i]*components[i]->pEOS->limits.Tmin;
    }
    return summer;
}
long double HelmholtzEOSMixtureBackend::calc_pmax(void)
{
    double summer = 0;
    for (unsigned int i = 0; i < components.size(); ++i)
    {
        summer += mole_fractions[i]*components[i]->pEOS->limits.pmax;
    }
    return summer;
}

void HelmholtzEOSMixtureBackend::update_TP_guessrho(long double T, long double p, long double rho_guess)
{
    double rho = solver_rho_Tp(T, p, rho_guess);
    update(DmolarT_INPUTS, rho, T);
}

void HelmholtzEOSMixtureBackend::mass_to_molar_inputs(long &input_pair, double &value1, double &value2)
{
    // Check if a mass based input, convert it to molar units

    switch(input_pair)
    {
        case DmassT_INPUTS: ///< Mass density in kg/m^3, Temperature in K
        case HmassT_INPUTS: ///< Enthalpy in J/kg, Temperature in K
        case SmassT_INPUTS: ///< Entropy in J/kg/K, Temperature in K
        case TUmass_INPUTS: ///< Temperature in K, Internal energy in J/kg
        case DmassP_INPUTS: ///< Mass density in kg/m^3, Pressure in Pa
        case HmassP_INPUTS: ///< Enthalpy in J/kg, Pressure in Pa
        case PSmass_INPUTS: ///< Pressure in Pa, Entropy in J/kg/K
        case PUmass_INPUTS: ///< Pressure in Pa, Internal energy in J/kg
        case HmassSmass_INPUTS: ///< Enthalpy in J/kg, Entropy in J/kg/K
        case SmassUmass_INPUTS: ///< Entropy in J/kg/K, Internal energy in J/kg
        case DmassHmass_INPUTS: ///< Mass density in kg/m^3, Enthalpy in J/kg
        case DmassSmass_INPUTS: ///< Mass density in kg/m^3, Entropy in J/kg/K
        case DmassUmass_INPUTS: ///< Mass density in kg/m^3, Internal energy in J/kg
        {
            // Set the cache value for the molar mass if it hasn't been set yet
            molar_mass();

            // Molar mass (just for compactness of the following switch)
            long double mm = static_cast<long double>(_molar_mass);

            switch(input_pair)
            {
                case DmassT_INPUTS: input_pair = DmolarT_INPUTS; value1 /= mm;  break;
                case HmassT_INPUTS: input_pair = HmolarT_INPUTS; value1 *= mm;  break;
                case SmassT_INPUTS: input_pair = SmolarT_INPUTS; value1 *= mm;  break;
                case TUmass_INPUTS: input_pair = TUmolar_INPUTS; value2 *= mm;  break;
                case DmassP_INPUTS: input_pair = DmolarP_INPUTS; value1 /= mm;  break;
                case HmassP_INPUTS: input_pair = HmolarP_INPUTS; value1 *= mm;  break;
                case PSmass_INPUTS: input_pair = PSmolar_INPUTS; value2 *= mm;  break;
                case PUmass_INPUTS: input_pair = PUmolar_INPUTS; value2 *= mm;  break;
                case HmassSmass_INPUTS: input_pair = HmolarSmolar_INPUTS; value1 *= mm; value2 *= mm;  break;
                case SmassUmass_INPUTS: input_pair = SmolarUmolar_INPUTS; value1 *= mm; value2 *= mm;  break;
                case DmassHmass_INPUTS: input_pair = DmolarHmolar_INPUTS; value1 /= mm; value2 *= mm;  break;
                case DmassSmass_INPUTS: input_pair = DmolarSmolar_INPUTS; value1 /= mm; value2 *= mm;  break;
                case DmassUmass_INPUTS: input_pair = DmolarUmolar_INPUTS; value1 /= mm; value2 *= mm;  break;
            }

        }
        default:
            return;
    }
}
void HelmholtzEOSMixtureBackend::update(long input_pair, double value1, double value2 )
{
    clear();

    if (is_pure_or_pseudopure == false && mole_fractions.size() == 0) {
        throw ValueError("Mole fractions must be set");
    }

    mass_to_molar_inputs(input_pair, value1, value2);

    // Set the mole-fraction weighted gas constant for the mixture
    // (or the pure/pseudo-pure fluid) if it hasn't been set yet
    gas_constant();

    // Reducing state
    calc_reducing_state();

    switch(input_pair)
    {
        case PT_INPUTS:
            _p = value1; _T = value2; FlashRoutines::PT_flash(*this); break;
        case DmolarT_INPUTS:
            _rhomolar = value1; _T = value2; FlashRoutines::DHSU_T_flash(*this, iDmolar); break;
        case SmolarT_INPUTS:
            _smolar = value1; _T = value2; FlashRoutines::DHSU_T_flash(*this, iSmolar); break;
        case HmolarT_INPUTS:
            _hmolar = value1; _T = value2; FlashRoutines::DHSU_T_flash(*this, iHmolar); break;
        case TUmolar_INPUTS:
            _T = value1; _umolar = value2; FlashRoutines::DHSU_T_flash(*this, iUmolar); break;
        case DmolarP_INPUTS:
            _rhomolar = value1; _p = value2; FlashRoutines::PHSU_D_flash(*this, iP); break;
        case DmolarHmolar_INPUTS:
            _rhomolar = value1; _hmolar = value2; FlashRoutines::PHSU_D_flash(*this, iHmolar); break;
        case DmolarSmolar_INPUTS:
            _rhomolar = value1; _smolar = value2; FlashRoutines::PHSU_D_flash(*this, iSmolar); break;
        case DmolarUmolar_INPUTS:
            _rhomolar = value1; _umolar = value2; FlashRoutines::PHSU_D_flash(*this, iUmolar); break;
        case HmolarP_INPUTS:
            _hmolar = value1; _p = value2; FlashRoutines::HSU_P_flash(*this, iHmolar); break;
        case PSmolar_INPUTS:
            _p = value1; _smolar = value2; FlashRoutines::HSU_P_flash(*this, iSmolar); break;
        case PUmolar_INPUTS:
            _p = value1; _umolar = value2; FlashRoutines::HSU_P_flash(*this, iUmolar); break;
        case QT_INPUTS:
            _Q = value1; _T = value2; FlashRoutines::QT_flash(*this); break;
        case PQ_INPUTS:
            _p = value1; _Q = value2; FlashRoutines::PQ_flash(*this); break;
        default:
            throw ValueError(format("This pair of inputs [%s] is not yet supported", get_input_pair_short_desc(input_pair).c_str()));
    }
    // Check the values that must always be set
    //if (_p < 0){ throw ValueError("p is less than zero");}
    if (!ValidNumber(_p)){ throw ValueError("p is not a valid number");}
    //if (_T < 0){ throw ValueError("T is less than zero");}
    if (!ValidNumber(_T)){ throw ValueError("T is not a valid number");}
    if (_rhomolar < 0){ throw ValueError("rhomolar is less than zero");}
    if (!ValidNumber(_rhomolar)){ throw ValueError("rhomolar is not a valid number");}
    if (!ValidNumber(_Q)){ throw ValueError("Q is not a valid number");}
    if (_phase == iphase_unknown){ 
            throw ValueError("_phase is unknown");
    }

    // Set the reduced variables
    _tau = _reducing.T/_T;
    _delta = _rhomolar/_reducing.rhomolar;
}

long double HelmholtzEOSMixtureBackend::calc_Bvirial()
{
    return 1/get_reducing().rhomolar*calc_alphar_deriv_nocache(0,1,mole_fractions,_tau,1e-12);
}
long double HelmholtzEOSMixtureBackend::calc_dBvirial_dT()
{
    long double dtau_dT =-get_reducing().T/pow(_T,2);
    return 1/get_reducing().rhomolar*calc_alphar_deriv_nocache(1,1,mole_fractions,_tau,1e-12)*dtau_dT;
}
long double HelmholtzEOSMixtureBackend::calc_Cvirial()
{
    return 1/pow(get_reducing().rhomolar,2)*calc_alphar_deriv_nocache(0,2,mole_fractions,_tau,1e-12);
}
long double HelmholtzEOSMixtureBackend::calc_dCvirial_dT()
{
    long double dtau_dT =-get_reducing().T/pow(_T,2);
    return 1/pow(get_reducing().rhomolar,2)*calc_alphar_deriv_nocache(1,2,mole_fractions,_tau,1e-12)*dtau_dT;
}
void HelmholtzEOSMixtureBackend::p_phase_determination_pure_or_pseudopure(int other, long double value, bool &saturation_called)
{
    /*
    Determine the phase given p and one other state variable
    */
    saturation_called = false;
    
    // Reference declaration to save indexing
    CoolPropFluid &component = *(components[0]);
    
    // Check supercritical pressure
    if (_p > _crit.p)
    {
        _Q = 1e9;
        switch (other)
        {
            case iT:
            {
                if (_T > _crit.T){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_liquid; return;
                }
            }
            case iDmolar:
            {
                if (_rhomolar < _crit.rhomolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_liquid; return;
                }
            }
            case iSmolar:
            {
                if (_smolar.pt() > _crit.smolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_liquid; return;
                }
            }
            case iHmolar:
            {
                if (_hmolar.pt() > _crit.hmolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_liquid; return;
                }
            }
            case iUmolar:
            {
                if (_umolar.pt() > _crit.umolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_liquid; return;
                }
            }
            default:
            {
                throw ValueError("supercritical pressure but other invalid for now");
            }
        }
    }
    // Check between triple point pressure and psat_max
    else if (_p > components[0]->pEOS->ptriple && _p < _crit.p)
    {
        // First try the ancillaries, use them to determine the state if you can
        
        // Calculate dew and bubble temps from the ancillaries (everything needs them)
        _TLanc = components[0]->ancillaries.pL.invert(_p);
        _TVanc = components[0]->ancillaries.pV.invert(_p);
        
        bool definitely_two_phase = false;
        
        // Try using the ancillaries for P,H,S if they are there
        switch (other)
        {
            case iT:
            {
                long double p_vap = 0.98*static_cast<double>(_pVanc);
                long double p_liq = 1.02*static_cast<double>(_pLanc);

                if (value < p_vap){
                    this->_phase = iphase_gas; _Q = -1000; return;
                }
                else if (value > p_liq){
                    this->_phase = iphase_liquid; _Q = 1000; return;
                }
                break;
            }
            case iHmolar:
            {
                // Ancillaries are h-h_anchor, so add back h_anchor
                long double h_liq = component.ancillaries.hL.evaluate(_TLanc) + component.EOSVector[0].hs_anchor.hmolar;
                long double h_liq_error_band = component.ancillaries.hL.get_max_abs_error();
                long double h_vap = h_liq + component.ancillaries.hLV.evaluate(_TLanc);
                long double h_vap_error_band = h_liq_error_band + component.ancillaries.hLV.get_max_abs_error();
                                
//                HelmholtzEOSMixtureBackend HEOS(components);
//                HEOS.update(QT_INPUTS, 1, _TLanc);
//                double h1 = HEOS.hmolar();
//                HEOS.update(QT_INPUTS, 0, _TLanc);
//                double h0 = HEOS.hmolar();
                
                // Check if in range given the accuracy of the fit
                if (value > h_vap + h_vap_error_band){
                    this->_phase = iphase_gas; _Q = -1000; return;
                }
                else if (value < h_liq - h_liq_error_band){
                    this->_phase = iphase_liquid; _Q = 1000; return;
                }
                else if (value > h_liq + h_liq_error_band && value < h_vap - h_vap_error_band){ definitely_two_phase = true;}
                break;
            }
            case iSmolar:
            {
                // Ancillaries are s-s_anchor, so add back s_anchor
                long double s_anchor = component.EOSVector[0].hs_anchor.smolar;
                long double s_liq = component.ancillaries.sL.evaluate(_TLanc) + s_anchor;
                long double s_liq_error_band = component.ancillaries.sL.get_max_abs_error();
                long double s_vap = s_liq + component.ancillaries.sLV.evaluate(_TVanc);
                long double s_vap_error_band = s_liq_error_band + component.ancillaries.sLV.get_max_abs_error();
                
                // Check if in range given the accuracy of the fit
                if (value > s_vap + s_vap_error_band){
                    this->_phase = iphase_gas; _Q = -1000; return;
                }
                else if (value < s_liq - s_liq_error_band){
                    this->_phase = iphase_liquid; _Q = 1000; return;
                }
                else if (value > s_liq + s_liq_error_band && value < s_vap - s_vap_error_band){ definitely_two_phase = true;}
				break;
            }
            case iUmolar:
            {
				// u = h-p/rho
				// Ancillaries are h-h_anchor, so add back h_anchor
                long double h_liq = component.ancillaries.hL.evaluate(_TLanc) + component.EOSVector[0].hs_anchor.hmolar;
                long double h_liq_error_band = component.ancillaries.hL.get_max_abs_error();
                long double h_vap = h_liq + component.ancillaries.hLV.evaluate(_TLanc);
                long double h_vap_error_band = h_liq_error_band + component.ancillaries.hLV.get_max_abs_error();
				long double rho_vap = component.ancillaries.rhoV.evaluate(_TVanc);
				long double rho_liq = component.ancillaries.rhoL.evaluate(_TLanc);
				long double u_liq = h_liq-_p/rho_liq;
				long double u_vap = h_vap-_p/rho_vap;
				long double u_liq_error_band = 1.5*h_liq_error_band; // Most of error is in enthalpy
				long double u_vap_error_band = 1.5*h_vap_error_band; // Most of error is in enthalpy
				
				// Check if in range given the accuracy of the fit
                if (value > u_vap + u_vap_error_band){
                    this->_phase = iphase_gas; _Q = -1000; return;
                }
                else if (value < u_liq - u_liq_error_band){
                    this->_phase = iphase_liquid; _Q = 1000; return;
                }
                else if (value > u_liq + u_liq_error_band && value < u_vap - u_vap_error_band){ definitely_two_phase = true;}
				break;
                
            }
            default:
            {
            }
        }
        
        // Now either density is an input, or an ancillary for h,s,u is missing
        // Always calculate the densities using the ancillaries
        if (!definitely_two_phase)
        {
            _rhoVanc = component.ancillaries.rhoV.evaluate(_TVanc);
            _rhoLanc = component.ancillaries.rhoL.evaluate(_TLanc);
            long double rho_vap = 0.95*static_cast<double>(_rhoVanc);
            long double rho_liq = 1.05*static_cast<double>(_rhoLanc);
            switch (other)
            {
                case iDmolar:
                {
                    if (value < rho_vap){
                        this->_phase = iphase_gas; return;
                    }
                    else if (value > rho_liq){
                        this->_phase = iphase_liquid; return;
                    }
                    break;
                }
            }
        }

        if (!is_pure_or_pseudopure){throw ValueError("possibly two-phase inputs not supported for pseudo-pure for now");}

        // Actually have to use saturation information sadly
        // For the given pressure, find the saturation state
        // Run the saturation routines to determine the saturation densities and pressures
        HelmholtzEOSMixtureBackend HEOS(components);
        HEOS._p = this->_p;
        HEOS._Q = 0; // ?? What is the best to do here? Doesn't matter for our purposes since pure fluid
        FlashRoutines::PQ_flash(HEOS);

        // We called the saturation routines, so HEOS.SatL and HEOS.SatV are now updated
        // with the saturated liquid and vapor values, which can therefore be used in
        // the other solvers
        saturation_called = true;
        
        long double Q;

        switch (other)
        {
            case iDmolar:
                Q = (1/value-1/HEOS.SatL->rhomolar())/(1/HEOS.SatV->rhomolar()-1/HEOS.SatL->rhomolar()); break;
            case iSmolar:
                Q = (value - HEOS.SatL->smolar())/(HEOS.SatV->smolar() - HEOS.SatL->smolar()); break;
            case iHmolar:
                Q = (value - HEOS.SatL->hmolar())/(HEOS.SatV->hmolar() - HEOS.SatL->hmolar()); break;
            case iUmolar:
                Q = (value - HEOS.SatL->umolar())/(HEOS.SatV->umolar() - HEOS.SatL->umolar()); break;
            default:
                throw ValueError(format("bad input for other"));
        }
        // Update the states
        this->SatL->update(DmolarT_INPUTS, HEOS.SatL->rhomolar(), HEOS.SatL->T());
        this->SatV->update(DmolarT_INPUTS, HEOS.SatV->rhomolar(), HEOS.SatV->T());
        if (Q < -100*DBL_EPSILON){
            this->_phase = iphase_liquid; _Q = -1000;  return;
        }
        else if (Q > 1+100*DBL_EPSILON){
            this->_phase = iphase_gas; _Q = 1000; return;
        }
        else{
            this->_phase = iphase_twophase;
        }
        
        _Q = Q;
        // Load the outputs
        _p = _Q*HEOS.SatV->p() + (1-_Q)*HEOS.SatL->p();
        _rhomolar = 1/(_Q/HEOS.SatV->rhomolar() + (1-_Q)/HEOS.SatL->rhomolar());
        return;
    }
    else if (_p < components[0]->pEOS->ptriple)
    {
        throw NotImplementedError(format("for now, we don't support p [%g Pa] below ptriple [%g Pa]",_p, components[0]->pEOS->ptriple));
    }
}
void HelmholtzEOSMixtureBackend::T_phase_determination_pure_or_pseudopure(int other, long double value)
{
    if (!ValidNumber(value)){
        throw ValueError(format("value to T_phase_determination_pure_or_pseudopure is invalid"));};
    // T is known, another input P, T, H, S, U is given (all molar)
    if (_T < _crit.T)
    {
        // Start to think about the saturation stuff
        // First try to use the ancillary equations if you are far enough away
        // You know how accurate the ancillary equations are thanks to using CoolProp code to refit them
        switch (other)
        {
            case iP:
            {
                _pLanc = components[0]->ancillaries.pL.evaluate(_T);
                _pVanc = components[0]->ancillaries.pV.evaluate(_T);
                long double p_vap = 0.98*static_cast<double>(_pVanc);
                long double p_liq = 1.02*static_cast<double>(_pLanc);

                if (value < p_vap){
                    this->_phase = iphase_gas; _Q = -1000; return;
                }
                else if (value > p_liq){
                    this->_phase = iphase_liquid; _Q = 1000; return;
                }
                break;
            }
            default:
            {
                // Always calculate the densities using the ancillaries
                _rhoVanc = components[0]->ancillaries.rhoV.evaluate(_T);
                _rhoLanc = components[0]->ancillaries.rhoL.evaluate(_T);
                long double rho_vap = 0.95*static_cast<double>(_rhoVanc);
                long double rho_liq = 1.05*static_cast<double>(_rhoLanc);
                switch (other)
                {
                    case iDmolar:
                    {
                        if (value < rho_vap){
                            this->_phase = iphase_gas; return;
                        }
                        else if (value > rho_liq){
                            this->_phase = iphase_liquid; return;
                        }
                        break;
                    }
                    default:
                    {
                        // If it is not density, update the states
                        SatV->update(DmolarT_INPUTS, rho_vap, _T);
                        SatL->update(DmolarT_INPUTS, rho_liq, _T);

                        // First we check ancillaries
                        switch (other)
                        {
                            case iSmolar:
                            {
                                if (value > SatV->calc_smolar()){
                                    this->_phase = iphase_gas; return;
                                }
                                if (value < SatL->calc_smolar()){
                                    this->_phase = iphase_liquid; return;
                                }
                                break;
                            }
                            case iHmolar:
                            {
                                if (value > SatV->calc_hmolar()){
                                    this->_phase = iphase_gas; return;
                                }
                                else if (value < SatL->calc_hmolar()){
                                    this->_phase = iphase_liquid; return;
                                }
                            }
                            case iUmolar:
                            {
                                if (value > SatV->calc_umolar()){
                                    this->_phase = iphase_gas; return;
                                }
                                else if (value < SatL->calc_umolar()){
                                    this->_phase = iphase_liquid; return;
                                }
                                break;
                            }
                            default:
                                throw ValueError(format("invalid input for other to T_phase_determination_pure_or_pseudopure"));
                        }
                    }
                }
            }
        }

        // Determine Q based on the input provided
        if (!is_pure_or_pseudopure){throw ValueError("possibly two-phase inputs not supported for pseudo-pure for now");}

        // Actually have to use saturation information sadly
        // For the given temperature, find the saturation state
        // Run the saturation routines to determine the saturation densities and pressures
        HelmholtzEOSMixtureBackend HEOS(components);
        SaturationSolvers::saturation_T_pure_options options;
        SaturationSolvers::saturation_T_pure(&HEOS, _T, options);

        long double Q;

        if (other == iP)
        {
            if (value > HEOS.SatL->p()*(100*DBL_EPSILON + 1)){
                this->_phase = iphase_liquid; _Q = -1000; return;
            }
            else if (value < HEOS.SatV->p()*(1 - 100*DBL_EPSILON)){
                this->_phase = iphase_gas; _Q = 1000; return;
            }
            else{
                throw ValueError(format("subcrit T, funny p"));
            }
        }

        switch (other)
        {
            case iDmolar:
                Q = (1/value-1/HEOS.SatL->rhomolar())/(1/HEOS.SatV->rhomolar()-1/HEOS.SatL->rhomolar()); break;
            case iSmolar:
                Q = (value - HEOS.SatL->smolar())/(HEOS.SatV->smolar() - HEOS.SatV->smolar()); break;
            case iHmolar:
                Q = (value - HEOS.SatL->hmolar())/(HEOS.SatV->hmolar() - HEOS.SatV->hmolar()); break;
            case iUmolar:
                Q = (value - HEOS.SatL->umolar())/(HEOS.SatV->umolar() - HEOS.SatV->umolar()); break;
            default:
                throw ValueError(format("bad input for other"));
        }

        if (Q < -100*DBL_EPSILON){
            this->_phase = iphase_liquid; _Q = -1000; return;
        }
        else if (Q > 1+100*DBL_EPSILON){
            this->_phase = iphase_gas; _Q = 1000; return;
        }
        else{
            this->_phase = iphase_twophase;
        }
        _Q = Q;
        // Load the outputs
        _p = _Q*HEOS.SatV->p() + (1-_Q)*HEOS.SatL->p();
        _rhomolar = 1/(_Q/HEOS.SatV->rhomolar() + (1-_Q)/HEOS.SatL->rhomolar());
        return;
    }
    else if (_T > _crit.T && _T > components[0]->pEOS->Ttriple)
    {
        _Q = 1e9;
        switch (other)
        {
            case iP:
            {
                if (_p > _crit.p){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_gas; return;
                }
            }
            case iDmolar:
            {
                if (_rhomolar > _crit.rhomolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_gas; return;
                }
            }
            case iSmolar:
            {
                if (_smolar.pt() > _crit.smolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_gas; return;
                }
            }
            case iHmolar:
            {
                if (_hmolar.pt() > _crit.hmolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_gas; return;
                }
            }
            case iUmolar:
            {
                if (_umolar.pt() > _crit.umolar){
                    this->_phase = iphase_supercritical; return;
                }
                else{
                    this->_phase = iphase_gas; return;
                }
            }
            default:
            {
                throw ValueError("supercritical temp but other invalid for now");
            }
        }
    }
    else
    {
        throw ValueError(format("For now, we don't support T [%g K] below Ttriple [%g K]", _T, components[0]->pEOS->Ttriple));
    }
}
//void HelmholtzEOSMixtureBackend::DmolarT_phase_determination_pure_or_pseudopure()
//{
//    if (_T < _crit.T)
//	{
//		// Start to think about the saturation stuff
//		// First try to use the ancillary equations if you are far enough away
//		// You know how accurate the ancillary equations are thanks to using CoolProp code to refit them
//        if (_rhomolar < 0.95*components[0]->ancillaries.rhoV.evaluate(_T)){
//            this->_phase = iphase_gas; return;
//		}
//        else if (_rhomolar > 1.05*components[0]->ancillaries.rhoL.evaluate(_T)){
//			this->_phase = iphase_liquid; return;
//		}
//		else{
//			// Actually have to use saturation information sadly
//			// For the given temperature, find the saturation state
//			// Run the saturation routines to determine the saturation densities and pressures
//            HelmholtzEOSMixtureBackend HEOS(components);
//            SaturationSolvers::saturation_T_pure_options options;
//            SaturationSolvers::saturation_T_pure(&HEOS, _T, options);
//
//            long double Q = (1/_rhomolar-1/HEOS.SatL->rhomolar())/(1/HEOS.SatV->rhomolar()-1/HEOS.SatL->rhomolar());
//			if (Q < -100*DBL_EPSILON){
//				this->_phase = iphase_liquid;
//			}
//			else if (Q > 1+100*DBL_EPSILON){
//				this->_phase = iphase_gas;
//			}
//			else{
//                this->_phase = iphase_twophase;
//            }
//            _Q = Q;
//             // Load the outputs
//            _p = _Q*HEOS.SatV->p() + (1-_Q)*HEOS.SatL->p();
//            _rhomolar = 1/(_Q/HEOS.SatV->rhomolar() + (1-_Q)/HEOS.SatL->rhomolar());
//            return;
//		}
//	}
//	// Now check the states above the critical temperature.
//
//    // Calculate the pressure if it is not already cached.
//	calc_pressure();
//
//    if (_T > _crit.T && _p > _crit.p){
//        this->_phase = iphase_supercritical; return;
//	}
//	else if (_T > _crit.T && _p < _crit.p){
//		this->_phase = iphase_gas; return;
//	}
//	else if (_T < _crit.T && _p > _crit.p){
//		this->_phase = iphase_liquid; return;
//	}
//	/*else if (p < params.ptriple){
//		return iphase_gas;
//	}*/
//	else{
//		throw ValueError(format("phase cannot be determined"));
//	}
//}

//void HelmholtzEOSMixtureBackend::PT_phase_determination()
//{
//    if (_T < _crit.T)
//	{
//		// Start to think about the saturation stuff
//		// First try to use the ancillary equations if you are far enough away
//		// Ancillary equations are good to within 1% in pressure in general
//		// Some industrial fluids might not be within 3%
//        if (_p > 1.05*components[0]->ancillaries.pL.evaluate(_T)){
//            this->_phase = iphase_liquid; return;
//		}
//        else if (_p < 0.95*components[0]->ancillaries.pV.evaluate(_T)){
//			this->_phase = iphase_gas; return;
//		}
//		else{
//            throw NotImplementedError("potentially two phase inputs not possible yet");
//			//// Actually have to use saturation information sadly
//			//// For the given temperature, find the saturation state
//			//// Run the saturation routines to determine the saturation densities and pressures
//			//// Use the passed in variables to save calls to the saturation routine so the values can be re-used again
//			//saturation_T(T, enabled_TTSE_LUT, pL, pV, rhoL, rhoV);
//			//double Q = (1/rho-1/rhoL)/(1/rhoV-1/rhoL);
//			//if (Q < -100*DBL_EPSILON){
//			//	this->_phase = iphase_liquid; return;
//			//}
//			//else if (Q > 1+100*DBL_EPSILON){
//			//	this->_phase = iphase_gas; return;
//			//}
//			//else{
//			//	this->_phase = iphase_twophase; return;
//			//}
//		}
//	}
//	// Now check the states above the critical temperature.
//    if (_T > _crit.T && _p > _crit.p){
//        this->_phase = iphase_supercritical; return;
//	}
//	else if (_T > _crit.T && _p < _crit.p){
//		this->_phase = iphase_gas; return;
//	}
//	else if (_T < _crit.T && _p > _crit.p){
//		this->_phase = iphase_liquid; return;
//	}
//	/*else if (p < params.ptriple){
//		return iphase_gas;
//	}*/
//	else{
//		throw ValueError(format("phase cannot be determined"));
//	}
//}

void get_dtau_ddelta(HelmholtzEOSMixtureBackend *HEOS, long double T, long double rho, int index, long double &dtau, long double &ddelta)
{
    long double rhor = HEOS->get_reducing().rhomolar,
                Tr = HEOS->get_reducing().T,
                dT_dtau = -pow(T, 2)/Tr,
                R = HEOS->gas_constant(),
                delta = rho/rhor,
                tau = Tr/T;

    switch (index)
    {
    case iT:
        dtau = dT_dtau; ddelta = 0; break;
    case iDmolar:
        dtau = 0; ddelta = rhor; break;
    case iP:
        {
        long double dalphar_dDelta = HEOS->calc_alphar_deriv_nocache(0, 1, HEOS->get_mole_fractions(), tau, delta);
        long double d2alphar_dDelta2 = HEOS->calc_alphar_deriv_nocache(0, 2, HEOS->get_mole_fractions(), tau, delta);
        long double d2alphar_dDelta_dTau = HEOS->calc_alphar_deriv_nocache(1, 1, HEOS->get_mole_fractions(), tau, delta);
        // dp/ddelta|tau
        ddelta = rhor*R*T*(1+2*delta*dalphar_dDelta+pow(delta, 2)*d2alphar_dDelta2);
        // dp/dtau|delta
        dtau = dT_dtau*rho*R*(1+delta*dalphar_dDelta-tau*delta*d2alphar_dDelta_dTau);
        break;
        }
    case iHmolar:
        // dh/dtau|delta
        dtau = dT_dtau*R*(-pow(tau,2)*(HEOS->d2alpha0_dTau2()+HEOS->d2alphar_dTau2()) + (1+delta*HEOS->dalphar_dDelta()-tau*delta*HEOS->d2alphar_dDelta_dTau()));
        // dh/ddelta|tau
        ddelta = rhor*T*R/rho*(tau*delta*HEOS->d2alphar_dDelta_dTau()+delta*HEOS->dalphar_dDelta()+pow(delta,2)*HEOS->d2alphar_dDelta2());
        break;
    case iSmolar:
        // ds/dtau|delta
        dtau = dT_dtau*R/T*(-pow(tau,2)*(HEOS->d2alpha0_dTau2()+HEOS->d2alphar_dTau2()));
        // ds/ddelta|tau
        ddelta = rhor*R/rho*(-(1+delta*HEOS->dalphar_dDelta()-tau*delta*HEOS->d2alphar_dDelta_dTau()));
        break;
    case iUmolar:
        // du/dtau|delta
        dtau = dT_dtau*R*(-pow(tau,2)*(HEOS->d2alpha0_dTau2()+HEOS->d2alphar_dTau2()));
        // du/ddelta|tau
        ddelta = rhor*HEOS->T()*R/rho*(tau*delta*HEOS->d2alphar_dDelta_dTau());
        break;
    case iTau:
        dtau = 1; ddelta = 0; break;
    case iDelta:
        dtau = 0; ddelta = 1; break;
    default:
        throw ValueError(format("input to get_dtau_ddelta[%s] is invalid",get_parameter_information(index,"short").c_str()));
    }
}
long double HelmholtzEOSMixtureBackend::calc_first_partial_deriv_nocache(long double T, long double rhomolar, int Of, int Wrt, int Constant)
{
    long double dOf_dtau, dOf_ddelta, dWrt_dtau, dWrt_ddelta, dConstant_dtau, dConstant_ddelta;

    get_dtau_ddelta(this, T, rhomolar, Of, dOf_dtau, dOf_ddelta);
    get_dtau_ddelta(this, T, rhomolar, Wrt, dWrt_dtau, dWrt_ddelta);
    get_dtau_ddelta(this, T, rhomolar, Constant, dConstant_dtau, dConstant_ddelta);

    return (dOf_dtau*dConstant_ddelta-dOf_ddelta*dConstant_dtau)/(dWrt_dtau*dConstant_ddelta-dWrt_ddelta*dConstant_dtau);
}
long double HelmholtzEOSMixtureBackend::calc_first_partial_deriv(int Of, int Wrt, int Constant)
{
    return calc_first_partial_deriv_nocache(_T, _rhomolar, Of, Wrt, Constant);
}

long double HelmholtzEOSMixtureBackend::calc_pressure_nocache(long double T, long double rhomolar)
{
    SimpleState reducing = calc_reducing_state_nocache(mole_fractions);
    long double delta = rhomolar/reducing.rhomolar;
    long double tau = reducing.T/T;

    // Calculate derivative if needed
    int nTau = 0, nDelta = 1;
    long double dalphar_dDelta = calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, tau, delta);

    // Get pressure
    return rhomolar*gas_constant()*T*(1+delta*dalphar_dDelta);
}
//long double HelmholtzEOSMixtureBackend::solver_for_T_given_rho_oneof_PHSU(long double T, long double value, int other, int rhomin, int rhomax)
//{
//
//}
long double HelmholtzEOSMixtureBackend::solver_for_rho_given_T_oneof_HSU(long double T, long double value, int other)
{
    long double ymelt, yc, ymin, y;

    // Define the residual to be driven to zero
    class solver_resid : public FuncWrapper1D
    {
    public:
        int other;
        long double T, value, r, eos, rhomolar;
        HelmholtzEOSMixtureBackend *HEOS;

        solver_resid(HelmholtzEOSMixtureBackend *HEOS, long double T, long double value, int other){
            this->HEOS = HEOS; this->T = T; this->value = value; this->other = other;
        };
        double call(double rhomolar){
            this->rhomolar = rhomolar;
            switch(other)
            {
            case iSmolar:
                eos = HEOS->calc_smolar_nocache(T,rhomolar); break;
            case iHmolar:
                eos = HEOS->calc_hmolar_nocache(T,rhomolar); break;
            case iUmolar:
                eos = HEOS->calc_umolar_nocache(T,rhomolar); break;
            default:
                throw ValueError(format("Input not supported"));
            }

            r = eos-value;
            return r;
        };
    };
    solver_resid resid(this, T, value, other);
    std::string errstring;

    // Supercritical temperature
    if (_T > _crit.T)
    {
        long double rhomelt = components[0]->triple_liquid.rhomolar;
        long double rhoc = components[0]->crit.rhomolar;
        long double rhomin = 1e-10;

        switch(other)
        {

            case iSmolar:
            {
                ymelt = calc_smolar_nocache(_T, rhomelt);
                yc = calc_smolar_nocache(_T, rhoc);
                ymin = calc_smolar_nocache(_T, rhomin);
                y = _smolar;
                break;
            }
            case iHmolar:
            {
                ymelt = calc_hmolar_nocache(_T, rhomelt);
                yc = calc_hmolar_nocache(_T, rhoc);
                ymin = calc_hmolar_nocache(_T, rhomin);
                y = _hmolar;
                break;
            }
            case iUmolar:
            {
                ymelt = calc_umolar_nocache(_T, rhomelt);
                yc = calc_umolar_nocache(_T, rhoc);
                ymin = calc_umolar_nocache(_T, rhomin);
                y = _umolar;
                break;
            }
            default:
                throw ValueError();
        }

        if (is_in_closed_range(ymelt, yc, y))
        {
            long double rhomolar = Brent(resid, rhomelt, rhoc, LDBL_EPSILON, 1e-12, 100, errstring);
            return rhomolar;
        }
        else if (is_in_closed_range(yc, ymin, y))
        {
            long double rhomolar = Brent(resid, rhoc, rhomin, LDBL_EPSILON, 1e-12, 100, errstring);
            return rhomolar;
        }
        else
        {
            throw ValueError();
        }
    }
    // Subcritical temperature liquid
    else if (_phase == iphase_liquid)
    {
        long double ymelt, yL, y;
        long double rhomelt = components[0]->triple_liquid.rhomolar;
        long double rhoL = static_cast<double>(_rhoLanc);

        switch(other)
        {
            case iSmolar:
            {
                ymelt = calc_smolar_nocache(_T, rhomelt);  yL = calc_smolar_nocache(_T, rhoL); y = _smolar; break;
            }
            case iHmolar:
            {
                ymelt = calc_hmolar_nocache(_T, rhomelt);  yL = calc_hmolar_nocache(_T, rhoL); y = _hmolar; break;
            }
            case iUmolar:
            {
                ymelt = calc_umolar_nocache(_T, rhomelt);  yL = calc_umolar_nocache(_T, rhoL); y = _umolar; break;
            }
            default:
                throw ValueError();
        }

        long double rhomolar_guess = (rhomelt-rhoL)/(ymelt-yL)*(y-yL) + rhoL;

        long double rhomolar = Secant(resid, rhomolar_guess, 0.0001*rhomolar_guess, 1e-12, 100, errstring);
        return rhomolar;
    }
    // Subcritical temperature gas
    else if (_phase == iphase_gas)
    {
        long double rhomin = 1e-14;
        long double rhoV = static_cast<double>(_rhoVanc);

        try
        {
            long double rhomolar = Brent(resid, rhomin, rhoV, LDBL_EPSILON, 1e-12, 100, errstring);
            return rhomolar;
        }
        catch(std::exception &)
        {
            throw ValueError();
        }
    }
}
long double HelmholtzEOSMixtureBackend::solver_rho_Tp(long double T, long double p, long double rhomolar_guess)
{
    int phase;

    // Define the residual to be driven to zero
    class solver_TP_resid : public FuncWrapper1D
    {
    public:
        long double T, p, r, peos, rhomolar, rhor, tau, R_u, delta, dalphar_dDelta;
        HelmholtzEOSMixtureBackend *HEOS;

        solver_TP_resid(HelmholtzEOSMixtureBackend *HEOS, long double T, long double p){
            this->HEOS = HEOS; this->T = T; this->p = p; this->rhor = HEOS->get_reducing().rhomolar;
            this->tau = HEOS->get_reducing().T/T; this->R_u = HEOS->gas_constant();
        };
        double call(double rhomolar){
            this->rhomolar = rhomolar;
            delta = rhomolar/rhor;
            dalphar_dDelta = HEOS->calc_alphar_deriv_nocache(0, 1, HEOS->get_mole_fractions(), tau, delta);
            peos = rhomolar*R_u*T*(1+delta*dalphar_dDelta);
            r = (peos-p)/p;
            return r;
        };
        double deriv(double rhomolar){
            long double d2alphar_dDelta2 = HEOS->calc_alphar_deriv_nocache(0, 2, HEOS->get_mole_fractions(), tau, delta);
            // dp/ddelta|tau / pspecified
            return R_u*T*(1+2*delta*dalphar_dDelta+pow(delta, 2)*d2alphar_dDelta2)/p;
        };
    };
    solver_TP_resid resid(this,T,p);
    std::string errstring;

    if (imposed_phase_index > -1)
        phase = imposed_phase_index;
    else
        phase = _phase;
    if (rhomolar_guess < 0) // Not provided
    {
        rhomolar_guess = solver_rho_Tp_SRK(T, p, phase);

        if (phase == iphase_gas && rhomolar_guess < 0)// If the guess is bad, probably high temperature, use ideal gas
        {
            rhomolar_guess = p/(gas_constant()*T);
        }
        else
        {
            if (phase == iphase_liquid)
            {
                _rhoLanc = components[0]->ancillaries.rhoL.evaluate(T);
                if (rhomolar_guess < static_cast<long double>(_rhoLanc)){
                    rhomolar_guess = static_cast<long double>(_rhoLanc);
                }
            }
        }
    }

    try{
        // First we try with Newton's method with analytic derivative
        double rhomolar = Newton(resid, rhomolar_guess, 1e-8, 100, errstring);
        if (!ValidNumber(rhomolar)){
            throw ValueError();
        }
        return rhomolar;
    }
    catch(std::exception &)
    {
        try{
            // Next we try with Secant method
            double rhomolar = Secant(resid, rhomolar_guess, 0.0001*rhomolar_guess, 1e-8, 100, errstring);
            if (!ValidNumber(rhomolar)){throw ValueError();}
            return rhomolar;
        }
        catch(std::exception &)
        {
			try{
				Secant(resid, rhomolar_guess, 0.0001*rhomolar_guess, 1e-8, 100, errstring);
			}
			catch(...){
				
				throw ValueError(format("solver_rho_Tp was unable to find a solution for T=%10Lg, p=%10Lg, with guess value %10Lg",T,p,rhomolar_guess));
			}
            return _HUGE;
        }
    }
}
long double HelmholtzEOSMixtureBackend::solver_rho_Tp_SRK(long double T, long double p, int phase)
{
    long double rhomolar, R_u = gas_constant(), a = 0, b = 0, k_ij = 0;

    for (std::size_t i = 0; i < components.size(); ++i)
    {
        long double Tci = components[i]->pEOS->reduce.T, pci = components[i]->pEOS->reduce.p, accentric_i = components[i]->pEOS->accentric;
        long double m_i = 0.480+1.574*accentric_i-0.176*pow(accentric_i, 2);
        long double b_i = 0.08664*R_u*Tci/pci;
        b += mole_fractions[i]*b_i;

        long double a_i = 0.42747*pow(R_u*Tci,2)/pci*pow(1+m_i*(1-sqrt(T/Tci)),2);

        for (std::size_t j = 0; j < components.size(); ++j)
        {
            long double Tcj = components[j]->pEOS->reduce.T, pcj = components[j]->pEOS->reduce.p, accentric_j = components[j]->pEOS->accentric;
            long double m_j = 0.480+1.574*accentric_j-0.176*pow(accentric_j, 2);

            long double a_j = 0.42747*pow(R_u*Tcj,2)/pcj*pow(1+m_j*(1-sqrt(T/Tcj)),2);

            if (i == j){
                k_ij = 0;
            }
            else{
                k_ij = 0;
            }

            a += mole_fractions[i]*mole_fractions[j]*sqrt(a_i*a_j)*(1-k_ij);
        }
    }

    long double A = a*p/pow(R_u*T,2);
    long double B = b*p/(R_u*T);

    //Solve the cubic for solutions for Z = p/(rho*R*T)
    double Z0, Z1, Z2; int Nsolns;
    solve_cubic(1, -1, A-B-B*B, -A*B, Nsolns, Z0, Z1, Z2);

    // Determine the guess value
    if (Nsolns == 1){
        rhomolar = p/(Z0*R_u*T);
    }
    else{
        long double rhomolar0 = p/(Z0*R_u*T);
        long double rhomolar1 = p/(Z1*R_u*T);
        long double rhomolar2 = p/(Z2*R_u*T);

        // Check if only one solution is positive, return the solution if that is the case
        if (rhomolar0  > 0 && rhomolar1 <= 0 && rhomolar2 <= 0){ return rhomolar0; }
        if (rhomolar0 <= 0 && rhomolar1 >  0 && rhomolar2 <= 0){ return rhomolar1; }
        if (rhomolar0 <= 0 && rhomolar1 <= 0 && rhomolar2  > 0){ return rhomolar2; }

        switch(phase)
        {
        case iphase_liquid:
            rhomolar = max3(rhomolar0, rhomolar1, rhomolar2); break;
        case iphase_gas:
            rhomolar = min3(rhomolar0, rhomolar1, rhomolar2); break;
        default:
            throw ValueError("Bad phase to solver_rho_Tp_SRK");
        };
    }
    return rhomolar;
}

long double HelmholtzEOSMixtureBackend::calc_pressure(void)
{
    // Calculate the reducing parameters
    _delta = _rhomolar/_reducing.rhomolar;
    _tau = _reducing.T/_T;

    // Calculate derivative if needed
    long double dar_dDelta = dalphar_dDelta();
    long double R_u = gas_constant();

    // Get pressure
    _p = _rhomolar*R_u*_T*(1+_delta.pt()*dar_dDelta);

    //std::cout << format("p: %13.12f %13.12f %10.9f %10.9f %10.9f %10.9f %g\n",_T,_rhomolar,_tau,_delta,mole_fractions[0],dar_dDelta,_p);
    //if (_p < 0){
    //    throw ValueError("Pressure is less than zero");
    //}

    return static_cast<long double>(_p);
}
long double HelmholtzEOSMixtureBackend::calc_hmolar_nocache(long double T, long double rhomolar)
{
    // Calculate the reducing parameters
    long double delta = rhomolar/_reducing.rhomolar;
    long double tau = _reducing.T/T;

    // Calculate derivatives if needed, or just use cached values
    // Calculate derivative if needed
    long double dar_dDelta = calc_alphar_deriv_nocache(0, 1, mole_fractions, tau, delta);
    long double dar_dTau = calc_alphar_deriv_nocache(1, 0, mole_fractions, tau, delta);
    long double da0_dTau = calc_alpha0_deriv_nocache(1, 0, mole_fractions, tau, delta, _reducing.T, _reducing.rhomolar);
    long double R_u = gas_constant();

    // Get molar enthalpy
    return R_u*T*(1 + tau*(da0_dTau+dar_dTau) + delta*dar_dDelta);
}
long double HelmholtzEOSMixtureBackend::calc_hmolar(void)
{
    if (isTwoPhase())
    {
        _hmolar = _Q*SatV->hmolar() + (1 - _Q)*SatL->hmolar();
        return static_cast<long double>(_hmolar);
    }
    else if (isHomogeneousPhase())
    {
            // Calculate the reducing parameters
        _delta = _rhomolar/_reducing.rhomolar;
        _tau = _reducing.T/_T;

        // Calculate derivatives if needed, or just use cached values
        long double da0_dTau = dalpha0_dTau();
        long double dar_dTau = dalphar_dTau();
        long double dar_dDelta = dalphar_dDelta();
        long double R_u = gas_constant();

        // Get molar enthalpy
        _hmolar = R_u*_T*(1 + _tau.pt()*(da0_dTau+dar_dTau) + _delta.pt()*dar_dDelta);

        return static_cast<long double>(_hmolar);
    }
    else{
        throw ValueError(format("phase is invalid"));
    }
}
long double HelmholtzEOSMixtureBackend::calc_smolar_nocache(long double T, long double rhomolar)
{
    // Calculate the reducing parameters
    long double delta = rhomolar/_reducing.rhomolar;
    long double tau = _reducing.T/T;

    // Calculate derivatives if needed, or just use cached values
    // Calculate derivative if needed
    long double dar_dTau = calc_alphar_deriv_nocache(1, 0, mole_fractions, tau, delta);
    long double ar = calc_alphar_deriv_nocache(0, 0, mole_fractions, tau, delta);
    long double da0_dTau = calc_alpha0_deriv_nocache(1, 0, mole_fractions, tau, delta, _reducing.T, _reducing.rhomolar);
    long double a0 = calc_alpha0_deriv_nocache(0, 0, mole_fractions, tau, delta, _reducing.T, _reducing.rhomolar);
    long double R_u = gas_constant();

    // Get molar entropy
    return R_u*(tau*(da0_dTau+dar_dTau) - a0 - ar);
}
long double HelmholtzEOSMixtureBackend::calc_smolar(void)
{
    if (isTwoPhase())
    {
        _smolar = _Q*SatV->smolar() + (1 - _Q)*SatL->smolar();
        return static_cast<long double>(_smolar);
    }
    else if (isHomogeneousPhase())
    {
        // Calculate the reducing parameters
        _delta = _rhomolar/_reducing.rhomolar;
        _tau = _reducing.T/_T;

        // Calculate derivatives if needed, or just use cached values
        long double da0_dTau = dalpha0_dTau();
        long double ar = alphar();
        long double a0 = alpha0();
        long double dar_dTau = dalphar_dTau();
        long double R_u = gas_constant();

        // Get molar entropy
        _smolar = R_u*(_tau.pt()*(da0_dTau+dar_dTau) - a0 - ar);

        return static_cast<long double>(_smolar);
    }
    else{
        throw ValueError(format("phase is invalid"));
    }
}
long double HelmholtzEOSMixtureBackend::calc_umolar_nocache(long double T, long double rhomolar)
{
    // Calculate the reducing parameters
    long double delta = rhomolar/_reducing.rhomolar;
    long double tau = _reducing.T/T;

    // Calculate derivatives
    long double dar_dTau = calc_alphar_deriv_nocache(1, 0, mole_fractions, tau, delta);
    long double da0_dTau = calc_alpha0_deriv_nocache(1, 0, mole_fractions, tau, delta, _reducing.T, _reducing.rhomolar);
    long double R_u = gas_constant();

    // Get molar internal energy
    return R_u*T*tau*(da0_dTau+dar_dTau);
}
long double HelmholtzEOSMixtureBackend::calc_umolar(void)
{
	if (isTwoPhase())
    {
        _umolar = _Q*SatV->umolar() + (1 - _Q)*SatL->umolar();
        return static_cast<long double>(_umolar);
    }
    else if (isHomogeneousPhase())
    {
		// Calculate the reducing parameters
		_delta = _rhomolar/_reducing.rhomolar;
		_tau = _reducing.T/_T;

		// Calculate derivatives if needed, or just use cached values
		long double da0_dTau = dalpha0_dTau();
		long double dar_dTau = dalphar_dTau();
		long double R_u = gas_constant();

		// Get molar internal energy
		_umolar = R_u*_T*_tau.pt()*(da0_dTau+dar_dTau);

		return static_cast<long double>(_umolar);
	}
}
long double HelmholtzEOSMixtureBackend::calc_cvmolar(void)
{
    // Calculate the reducing parameters
    _delta = _rhomolar/_reducing.rhomolar;
    _tau = _reducing.T/_T;

    // Calculate derivatives if needed, or just use cached values
    long double d2ar_dTau2 = d2alphar_dTau2();
    long double d2a0_dTau2 = d2alpha0_dTau2();
    long double R_u = static_cast<double>(_gas_constant);

    // Get cv
    _cvmolar = -R_u*pow(_tau.pt(),2)*(d2ar_dTau2 + d2a0_dTau2);

    return static_cast<double>(_cvmolar);
}
long double HelmholtzEOSMixtureBackend::calc_cpmolar(void)
{
    // Calculate the reducing parameters
    _delta = _rhomolar/_reducing.rhomolar;
    _tau = _reducing.T/_T;

    // Calculate derivatives if needed, or just use cached values
    long double d2a0_dTau2 = d2alpha0_dTau2();
    long double dar_dDelta = dalphar_dDelta();
    long double d2ar_dDelta2 = d2alphar_dDelta2();
    long double d2ar_dDelta_dTau = d2alphar_dDelta_dTau();
    long double d2ar_dTau2 = d2alphar_dTau2();
    long double R_u = static_cast<double>(_gas_constant);

    // Get cp
    _cpmolar = R_u*(-pow(_tau.pt(),2)*(d2ar_dTau2 + d2a0_dTau2)+pow(1+_delta.pt()*dar_dDelta-_delta.pt()*_tau.pt()*d2ar_dDelta_dTau,2)/(1+2*_delta.pt()*dar_dDelta+pow(_delta.pt(),2)*d2ar_dDelta2));

    return static_cast<double>(_cpmolar);
}
long double HelmholtzEOSMixtureBackend::calc_cpmolar_idealgas(void)
{
    // Calculate the reducing parameters
    _delta = _rhomolar/_reducing.rhomolar;
    _tau = _reducing.T/_T;

    // Calculate derivatives if needed, or just use cached values
    long double d2a0_dTau2 = d2alpha0_dTau2();
    long double R_u = static_cast<double>(_gas_constant);

    // Get cp of the ideal gas
    return R_u*-pow(_tau.pt(),2)*d2a0_dTau2;
}
long double HelmholtzEOSMixtureBackend::calc_speed_sound(void)
{
    // Calculate the reducing parameters
    _delta = _rhomolar/_reducing.rhomolar;
    _tau = _reducing.T/_T;

    // Calculate derivatives if needed, or just use cached values
    long double d2a0_dTau2 = d2alpha0_dTau2();
    long double dar_dDelta = dalphar_dDelta();
    long double d2ar_dDelta2 = d2alphar_dDelta2();
    long double d2ar_dDelta_dTau = d2alphar_dDelta_dTau();
    long double d2ar_dTau2 = d2alphar_dTau2();
    long double R_u = gas_constant();
    long double mm = molar_mass();

    // Get speed of sound
    _speed_sound = sqrt(R_u*_T/mm*(1+2*_delta.pt()*dar_dDelta+pow(_delta.pt(),2)*d2ar_dDelta2 - pow(1+_delta.pt()*dar_dDelta-_delta.pt()*_tau.pt()*d2ar_dDelta_dTau,2)/(pow(_tau.pt(),2)*(d2ar_dTau2 + d2a0_dTau2))));

    return static_cast<double>(_speed_sound);
}

long double HelmholtzEOSMixtureBackend::calc_fugacity_coefficient(int i)
{
    return exp(mixderiv_ln_fugacity_coefficient(i));
}

SimpleState HelmholtzEOSMixtureBackend::calc_reducing_state_nocache(const std::vector<long double> & mole_fractions)
{
    SimpleState reducing;
    if (is_pure_or_pseudopure){
        reducing = components[0]->pEOS->reduce;

    }
    else{
        reducing.T = Reducing.p->Tr(mole_fractions);
        reducing.rhomolar = Reducing.p->rhormolar(mole_fractions);
    }
    return reducing;
}
void HelmholtzEOSMixtureBackend::calc_reducing_state(void)
{
    _reducing = calc_reducing_state_nocache(mole_fractions);
    _crit = _reducing;
}
long double HelmholtzEOSMixtureBackend::calc_alphar_deriv_nocache(const int nTau, const int nDelta, const std::vector<long double> &mole_fractions, const long double &tau, const long double &delta)
{
    if (is_pure_or_pseudopure)
    {
        if (nTau == 0 && nDelta == 0){
            return components[0]->pEOS->baser(tau, delta);
        }
        else if (nTau == 0 && nDelta == 1){
            return components[0]->pEOS->dalphar_dDelta(tau, delta);
        }
        else if (nTau == 1 && nDelta == 0){
            return components[0]->pEOS->dalphar_dTau(tau, delta);
        }
        else if (nTau == 0 && nDelta == 2){
            return components[0]->pEOS->d2alphar_dDelta2(tau, delta);
        }
        else if (nTau == 1 && nDelta == 1){
            return components[0]->pEOS->d2alphar_dDelta_dTau(tau, delta);
        }
        else if (nTau == 2 && nDelta == 0){
            return components[0]->pEOS->d2alphar_dTau2(tau, delta);
        }
        else if (nTau == 0 && nDelta == 3){
            return components[0]->pEOS->d3alphar_dDelta3(tau, delta);
        }
        else if (nTau == 1 && nDelta == 2){
            return components[0]->pEOS->d3alphar_dDelta2_dTau(tau, delta);
        }
        else if (nTau == 2 && nDelta == 1){
            return components[0]->pEOS->d3alphar_dDelta_dTau2(tau, delta);
        }
        else if (nTau == 3 && nDelta == 0){
            return components[0]->pEOS->d3alphar_dTau3(tau, delta);
        }
        else
        {
            throw ValueError();
        }
    }
    else{

        std::size_t N = mole_fractions.size();
        long double summer = 0;
        if (nTau == 0 && nDelta == 0){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->baser(tau, delta); }
            return summer + Excess.alphar(tau, delta, mole_fractions);
        }
        else if (nTau == 0 && nDelta == 1){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->dalphar_dDelta(tau, delta); }
            return summer + Excess.dalphar_dDelta(tau, delta, mole_fractions);
        }
        else if (nTau == 1 && nDelta == 0){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->dalphar_dTau(tau, delta); }
            return summer + Excess.dalphar_dTau(tau, delta, mole_fractions);
        }
        else if (nTau == 0 && nDelta == 2){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d2alphar_dDelta2(tau, delta); }
            return summer + Excess.d2alphar_dDelta2(tau, delta, mole_fractions);
        }
        else if (nTau == 1 && nDelta == 1){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d2alphar_dDelta_dTau(tau, delta); }
            return summer + Excess.d2alphar_dDelta_dTau(tau, delta, mole_fractions);
        }
        else if (nTau == 2 && nDelta == 0){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d2alphar_dTau2(tau, delta); }
            return summer + Excess.d2alphar_dTau2(tau, delta, mole_fractions);
        }
        /*else if (nTau == 0 && nDelta == 3){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d3alphar_dDelta3(tau, delta); }
            return summer + pExcess.d3alphar_dDelta3(tau, delta);
        }
        else if (nTau == 1 && nDelta == 2){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d3alphar_dDelta2_dTau(tau, delta); }
            return summer + pExcess.d3alphar_dDelta2_dTau(tau, delta);
        }
        else if (nTau == 2 && nDelta == 1){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d3alphar_dDelta_dTau2(tau, delta); }
            return summer + pExcess.d3alphar_dDelta_dTau2(tau, delta);
        }
        else if (nTau == 3 && nDelta == 0){
            for (unsigned int i = 0; i < N; ++i){ summer += mole_fractions[i]*components[i]->pEOS->d3alphar_dTau3(tau, delta); }
            return summer + pExcess.d3alphar_dTau3(tau, delta);
        }*/
        else
        {
            throw ValueError();
        }
    }
}
long double HelmholtzEOSMixtureBackend::calc_alpha0_deriv_nocache(const int nTau, const int nDelta, const std::vector<long double> &mole_fractions,
                                                                  const long double &tau, const long double &delta, const long double &Tr, const long double &rhor)
{
    long double val;
    if (is_pure_or_pseudopure)
    {
        if (nTau == 0 && nDelta == 0){
            val = components[0]->pEOS->base0(tau, delta);
        }
        else if (nTau == 0 && nDelta == 1){
            val = components[0]->pEOS->dalpha0_dDelta(tau, delta);
        }
        else if (nTau == 1 && nDelta == 0){
            val = components[0]->pEOS->dalpha0_dTau(tau, delta);
        }
        else if (nTau == 0 && nDelta == 2){
            val = components[0]->pEOS->d2alpha0_dDelta2(tau, delta);
        }
        else if (nTau == 1 && nDelta == 1){
            val = components[0]->pEOS->d2alpha0_dDelta_dTau(tau, delta);
        }
        else if (nTau == 2 && nDelta == 0){
            val = components[0]->pEOS->d2alpha0_dTau2(tau, delta);
        }
        else if (nTau == 0 && nDelta == 3){
            val = components[0]->pEOS->d3alpha0_dDelta3(tau, delta);
        }
        else if (nTau == 1 && nDelta == 2){
            val = components[0]->pEOS->d3alpha0_dDelta2_dTau(tau, delta);
        }
        else if (nTau == 2 && nDelta == 1){
            val = components[0]->pEOS->d3alpha0_dDelta_dTau2(tau, delta);
        }
        else if (nTau == 3 && nDelta == 0){
            val = components[0]->pEOS->d3alpha0_dTau3(tau, delta);
        }
        else
        {
            throw ValueError();
        }
        if (!ValidNumber(val)){
           //calc_alpha0_deriv_nocache(nTau,nDelta,mole_fractions,tau,delta,Tr,rhor);
           throw ValueError(format("calc_alpha0_deriv_nocache returned invalid number with inputs nTau: %d, nDelta: %d", nTau, nDelta));
        }
        else{
            return val;
        }
    }
    else{
        // See Table B5, GERG 2008 from Kunz Wagner, JCED, 2012
        std::size_t N = mole_fractions.size();
        long double summer = 0;
        long double tau_i, delta_i, rho_ci, T_ci;
        for (unsigned int i = 0; i < N; ++i){
            rho_ci = components[i]->pEOS->reduce.rhomolar;
            T_ci = components[i]->pEOS->reduce.T;
            tau_i = T_ci*tau/Tr;
            delta_i = delta*rhor/rho_ci;

            if (nTau == 0 && nDelta == 0){
                summer += mole_fractions[i]*(components[i]->pEOS->base0(tau_i, delta_i)+log(mole_fractions[i]));
            }
            else if (nTau == 0 && nDelta == 1){
                summer += mole_fractions[i]*rhor/rho_ci*components[i]->pEOS->dalpha0_dDelta(tau_i, delta_i);
            }
            else if (nTau == 1 && nDelta == 0){
                summer += mole_fractions[i]*T_ci/Tr*components[i]->pEOS->dalpha0_dTau(tau_i, delta_i);
            }
            else if (nTau == 0 && nDelta == 2){
                summer += mole_fractions[i]*pow(rhor/rho_ci,2)*components[i]->pEOS->d2alpha0_dDelta2(tau_i, delta_i);
            }
            else if (nTau == 1 && nDelta == 1){
                summer += mole_fractions[i]*rhor/rho_ci*T_ci/Tr*components[i]->pEOS->d2alpha0_dDelta_dTau(tau_i, delta_i);
            }
            else if (nTau == 2 && nDelta == 0){
                summer += mole_fractions[i]*pow(T_ci/Tr,2)*components[i]->pEOS->d2alpha0_dTau2(tau_i, delta_i);
            }
            else
            {
                throw ValueError();
            }
        }
        return summer;
    }
}
long double HelmholtzEOSMixtureBackend::calc_alphar(void)
{
    const int nTau = 0, nDelta = 0;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}
long double HelmholtzEOSMixtureBackend::calc_dalphar_dDelta(void)
{
    const int nTau = 0, nDelta = 1;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}
long double HelmholtzEOSMixtureBackend::calc_dalphar_dTau(void)
{
    const int nTau = 1, nDelta = 0;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}
long double HelmholtzEOSMixtureBackend::calc_d2alphar_dTau2(void)
{
    const int nTau = 2, nDelta = 0;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}
long double HelmholtzEOSMixtureBackend::calc_d2alphar_dDelta_dTau(void)
{
    const int nTau = 1, nDelta = 1;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}
long double HelmholtzEOSMixtureBackend::calc_d2alphar_dDelta2(void)
{
    const int nTau = 0, nDelta = 2;
    return calc_alphar_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta);
}

long double HelmholtzEOSMixtureBackend::calc_alpha0(void)
{
    const int nTau = 0, nDelta = 0;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}
long double HelmholtzEOSMixtureBackend::calc_dalpha0_dDelta(void)
{
    const int nTau = 0, nDelta = 1;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}
long double HelmholtzEOSMixtureBackend::calc_dalpha0_dTau(void)
{
    const int nTau = 1, nDelta = 0;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}
long double HelmholtzEOSMixtureBackend::calc_d2alpha0_dDelta2(void)
{
    const int nTau = 0, nDelta = 2;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}
long double HelmholtzEOSMixtureBackend::calc_d2alpha0_dDelta_dTau(void)
{
    const int nTau = 1, nDelta = 1;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}
long double HelmholtzEOSMixtureBackend::calc_d2alpha0_dTau2(void)
{
    const int nTau = 2, nDelta = 0;
    return calc_alpha0_deriv_nocache(nTau, nDelta, mole_fractions, _tau, _delta, _reducing.T, _reducing.rhomolar);
}


long double HelmholtzEOSMixtureBackend::mixderiv_dalphar_dxi(int i)
{
    return components[i]->pEOS->baser(_tau, _delta) + Excess.dalphar_dxi(_tau, _delta, mole_fractions, i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d2alphar_dxi_dTau(int i)
{
    return components[i]->pEOS->dalphar_dTau(_tau, _delta) + Excess.d2alphar_dxi_dTau(_tau, _delta, mole_fractions, i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d2alphar_dxi_dDelta(int i)
{
    return components[i]->pEOS->dalphar_dDelta(_tau, _delta) + Excess.d2alphar_dxi_dDelta(_tau, _delta, mole_fractions, i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d2alphardxidxj(int i, int j)
{
    return 0                           + Excess.d2alphardxidxj(_tau, _delta, mole_fractions, i, j);
}

long double HelmholtzEOSMixtureBackend::mixderiv_ln_fugacity_coefficient(int i)
{
    return alphar() + mixderiv_ndalphar_dni__constT_V_nj(i)-log(1+_delta.pt()*dalphar_dDelta());
}
long double HelmholtzEOSMixtureBackend::mixderiv_dln_fugacity_coefficient_dT__constrho_n(int i)
{
    double dtau_dT = -_tau.pt()/_T; //[1/K]
    return (dalphar_dTau() + mixderiv_d_ndalphardni_dTau(i)-1/(1+_delta.pt()*dalphar_dDelta())*(_delta.pt()*d2alphar_dDelta_dTau()))*dtau_dT;
}
long double HelmholtzEOSMixtureBackend::mixderiv_dln_fugacity_coefficient_drho__constT_n(int i)
{
    double ddelta_drho = 1/_reducing.rhomolar; //[m^3/mol]
    return (dalphar_dDelta() + mixderiv_d_ndalphardni_dDelta(i)-1/(1+_delta.pt()*dalphar_dDelta())*(_delta.pt()*d2alphar_dDelta2()+dalphar_dDelta()))*ddelta_drho;
}
long double HelmholtzEOSMixtureBackend::mixderiv_dnalphar_dni__constT_V_nj(int i)
{
    // GERG Equation 7.42
    return alphar() + mixderiv_ndalphar_dni__constT_V_nj(i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d2nalphar_dni_dT(int i)
{
    return -_tau.pt()/_T*(dalphar_dTau() + mixderiv_d_ndalphardni_dTau(i));
}
long double HelmholtzEOSMixtureBackend::mixderiv_dln_fugacity_coefficient_dT__constp_n(int i)
{
    double T = _reducing.T/_tau.pt();
    long double R_u = static_cast<long double>(_gas_constant);
    return mixderiv_d2nalphar_dni_dT(i) + 1/T-mixderiv_partial_molar_volume(i)/(R_u*T)*mixderiv_dpdT__constV_n();
}
long double HelmholtzEOSMixtureBackend::mixderiv_partial_molar_volume(int i)
{
    return -mixderiv_ndpdni__constT_V_nj(i)/mixderiv_ndpdV__constT_n();
}

long double HelmholtzEOSMixtureBackend::mixderiv_dln_fugacity_coefficient_dp__constT_n(int i)
{
    // GERG equation 7.30
    long double R_u = static_cast<long double>(_gas_constant);
    double partial_molar_volume = mixderiv_partial_molar_volume(i); // [m^3/mol]
    double term1 = partial_molar_volume/(R_u*_T); // m^3/mol/(N*m)*mol = m^2/N = 1/Pa
    double term2 = 1.0/p();
    return term1 - term2;
}

long double HelmholtzEOSMixtureBackend::mixderiv_dln_fugacity_coefficient_dxj__constT_p_xi(int i, int j)
{
    // Gernert 3.115
    long double R_u = static_cast<long double>(_gas_constant);
    // partial molar volume is -dpdn/dpdV, so need to flip the sign here
    return mixderiv_d2nalphar_dni_dxj__constT_V(i,j) - mixderiv_partial_molar_volume(i)/(R_u*_T)*mixderiv_dpdxj__constT_V_xi(j);
}
long double HelmholtzEOSMixtureBackend::mixderiv_dpdxj__constT_V_xi(int j)
{
    // Gernert 3.130
    long double R_u = static_cast<long double>(_gas_constant);
    return _rhomolar*R_u*_T*(mixderiv_ddelta_dxj__constT_V_xi(j)*dalphar_dDelta()+_delta.pt()*mixderiv_d_dalpharddelta_dxj__constT_V_xi(j));
}

long double HelmholtzEOSMixtureBackend::mixderiv_d_dalpharddelta_dxj__constT_V_xi(int j)
{
    // Gernert Equation 3.134 (Catch test provided)
    return d2alphar_dDelta2()*mixderiv_ddelta_dxj__constT_V_xi(j)
         + d2alphar_dDelta_dTau()*mixderiv_dtau_dxj__constT_V_xi(j)
         + mixderiv_d2alphar_dxi_dDelta(j);
}

long double HelmholtzEOSMixtureBackend::mixderiv_dalphar_dxj__constT_V_xi(int j)
{
    //Gernert 3.119 (Catch test provided)
    return dalphar_dDelta()*mixderiv_ddelta_dxj__constT_V_xi(j)+dalphar_dTau()*mixderiv_dtau_dxj__constT_V_xi(j)+mixderiv_dalphar_dxi(j);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d_ndalphardni_dxj__constT_V_xi(int i, int j)
{
    // Gernert 3.118
    return mixderiv_d_ndalphardni_dxj__constdelta_tau_xi(i,j)
          + mixderiv_ddelta_dxj__constT_V_xi(j)*mixderiv_d_ndalphardni_dDelta(i)
          + mixderiv_dtau_dxj__constT_V_xi(j)*mixderiv_d_ndalphardni_dTau(i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_ddelta_dxj__constT_V_xi(int j)
{
    // Gernert 3.121 (Catch test provided)
    return -_delta.pt()/_reducing.rhomolar*Reducing.p->drhormolardxi__constxj(mole_fractions,j);
}
long double HelmholtzEOSMixtureBackend::mixderiv_dtau_dxj__constT_V_xi(int j)
{
    // Gernert 3.122 (Catch test provided)
    return 1/_T*Reducing.p->dTrdxi__constxj(mole_fractions,j);
}

long double HelmholtzEOSMixtureBackend::mixderiv_dpdT__constV_n()
{
    long double R_u = static_cast<long double>(_gas_constant);
    return _rhomolar*R_u*(1+_delta.pt()*dalphar_dDelta()-_delta.pt()*_tau.pt()*d2alphar_dDelta_dTau());
}
long double HelmholtzEOSMixtureBackend::mixderiv_dpdrho__constT_n()
{
    long double R_u = static_cast<long double>(_gas_constant);
    return R_u*_T*(1+2*_delta.pt()*dalphar_dDelta()+pow(_delta.pt(),2)*d2alphar_dDelta2());
}
long double HelmholtzEOSMixtureBackend::mixderiv_ndpdV__constT_n()
{
    long double R_u = static_cast<long double>(_gas_constant);
    return -pow(_rhomolar,2)*R_u*_T*(1+2*_delta.pt()*dalphar_dDelta()+pow(_delta.pt(),2)*d2alphar_dDelta2());
}
long double HelmholtzEOSMixtureBackend::mixderiv_ndpdni__constT_V_nj(int i)
{
    // Eqn 7.64 and 7.63
    long double R_u = static_cast<long double>(_gas_constant);
    double ndrhorbar_dni__constnj = Reducing.p->ndrhorbardni__constnj(mole_fractions,i);
    double ndTr_dni__constnj = Reducing.p->ndTrdni__constnj(mole_fractions,i);
    double summer = 0;
    for (unsigned int k = 0; k < mole_fractions.size(); ++k)
    {
        summer += mole_fractions[k]*mixderiv_d2alphar_dxi_dDelta(k);
    }
    double nd2alphar_dni_dDelta = _delta.pt()*d2alphar_dDelta2()*(1-1/_reducing.rhomolar*ndrhorbar_dni__constnj)+_tau.pt()*d2alphar_dDelta_dTau()/_reducing.T*ndTr_dni__constnj+mixderiv_d2alphar_dxi_dDelta(i)-summer;
    return _rhomolar*R_u*_T*(1+_delta.pt()*dalphar_dDelta()*(2-1/_reducing.rhomolar*ndrhorbar_dni__constnj)+_delta.pt()*nd2alphar_dni_dDelta);
}

long double HelmholtzEOSMixtureBackend::mixderiv_ndalphar_dni__constT_V_nj(int i)
{
    double term1 = _delta.pt()*dalphar_dDelta()*(1-1/_reducing.rhomolar*Reducing.p->ndrhorbardni__constnj(mole_fractions,i));
    double term2 = _tau.pt()*dalphar_dTau()*(1/_reducing.T)*Reducing.p->ndTrdni__constnj(mole_fractions,i);

    double s = 0;
    for (unsigned int k = 0; k < mole_fractions.size(); k++)
    {
        s += mole_fractions[k]*mixderiv_dalphar_dxi(k);
    }
    double term3 = mixderiv_dalphar_dxi(i);
    return term1 + term2 + term3 - s;
}
long double HelmholtzEOSMixtureBackend::mixderiv_ndln_fugacity_coefficient_dnj__constT_p(int i, int j)
{
    long double R_u = static_cast<long double>(_gas_constant);
    return mixderiv_nd2nalphardnidnj__constT_V(j, i) + 1 - mixderiv_partial_molar_volume(j)/(R_u*_T)*mixderiv_ndpdni__constT_V_nj(i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_nddeltadni__constT_V_nj(int i)
{
    return _delta.pt()-_delta.pt()/_reducing.rhomolar*Reducing.p->ndrhorbardni__constnj(mole_fractions, i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_ndtaudni__constT_V_nj(int i)
{
    return _tau.pt()/_reducing.T*Reducing.p->ndTrdni__constnj(mole_fractions, i);
}
long double HelmholtzEOSMixtureBackend::mixderiv_d_ndalphardni_dxj__constdelta_tau_xi(int i, int j)
{
    double line1 = _delta.pt()*mixderiv_d2alphar_dxi_dDelta(j)*(1-1/_reducing.rhomolar*Reducing.p->ndrhorbardni__constnj(mole_fractions, i));
    double line2 = -_delta.pt()*dalphar_dDelta()*(1/_reducing.rhomolar)*(Reducing.p->d_ndrhorbardni_dxj__constxi(mole_fractions, i, j)-1/_reducing.rhomolar*Reducing.p->drhormolardxi__constxj(mole_fractions,j)*Reducing.p->ndrhorbardni__constnj(mole_fractions,i));
    double line3 = _tau.pt()*mixderiv_d2alphar_dxi_dTau(j)*(1/_reducing.T)*Reducing.p->ndTrdni__constnj(mole_fractions, i);
    double line4 = _tau.pt()*dalphar_dTau()*(1/_reducing.T)*(Reducing.p->d_ndTrdni_dxj__constxi(mole_fractions,i,j)-1/_reducing.T*Reducing.p->dTrdxi__constxj(mole_fractions,j)*Reducing.p->ndTrdni__constnj(mole_fractions, i));
    double s = 0;
    for (unsigned int m = 0; m < mole_fractions.size(); m++)
    {
        s += mole_fractions[m]*mixderiv_d2alphardxidxj(j,m);
    }
    double line5 = mixderiv_d2alphardxidxj(i,j)-mixderiv_dalphar_dxi(j)-s;
    return line1+line2+line3+line4+line5;
}
long double HelmholtzEOSMixtureBackend::mixderiv_nd2nalphardnidnj__constT_V(int i, int j)
{
    double line0 = mixderiv_ndalphar_dni__constT_V_nj(j); // First term from 7.46
    double line1 = mixderiv_d_ndalphardni_dDelta(i)*mixderiv_nddeltadni__constT_V_nj(j);
    double line2 = mixderiv_d_ndalphardni_dTau(i)*mixderiv_ndtaudni__constT_V_nj(j);
    double summer = 0;
    for (unsigned int k = 0; k < mole_fractions.size(); k++)
    {
        summer += mole_fractions[k]*mixderiv_d_ndalphardni_dxj__constdelta_tau_xi(i, k);
    }
    double line3 = mixderiv_d_ndalphardni_dxj__constdelta_tau_xi(i, j)-summer;
    return line0 + line1 + line2 + line3;
}
long double HelmholtzEOSMixtureBackend::mixderiv_d_ndalphardni_dDelta(int i)
{
    // The first line
    double term1 = (_delta.pt()*d2alphar_dDelta2()+dalphar_dDelta())*(1-1/_reducing.rhomolar*Reducing.p->ndrhorbardni__constnj(mole_fractions, i));

    // The second line
    double term2 = _tau.pt()*d2alphar_dDelta_dTau()*(1/_reducing.T)*Reducing.p->ndTrdni__constnj(mole_fractions, i);

    // The third line
    double term3 = mixderiv_d2alphar_dxi_dDelta(i);
    for (unsigned int k = 0; k < mole_fractions.size(); k++)
    {
        term3 -= mole_fractions[k]*mixderiv_d2alphar_dxi_dDelta(k);
    }
    return term1 + term2 + term3;
}

long double HelmholtzEOSMixtureBackend::mixderiv_d_ndalphardni_dTau(int i)
{
    // The first line
    double term1 = _delta.pt()*d2alphar_dDelta_dTau()*(1-1/_reducing.rhomolar*Reducing.p->ndrhorbardni__constnj(mole_fractions, i));

    // The second line
    double term2 = (_tau.pt()*d2alphar_dTau2()+dalphar_dTau())*(1/_reducing.T)*Reducing.p->ndTrdni__constnj(mole_fractions, i);

    // The third line
    double term3 = mixderiv_d2alphar_dxi_dTau(i);
    for (unsigned int k = 0; k < mole_fractions.size(); k++)
    {
        term3 -= mole_fractions[k]*mixderiv_d2alphar_dxi_dTau(k);
    }
    return term1 + term2 + term3;
}


} /* namespace CoolProp */
