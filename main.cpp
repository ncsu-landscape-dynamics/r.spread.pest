/*
 * SOD model
 *
 * Copyright (C) 2015-2017 by the authors.
 *
 * Authors: Zexi Chen (zchen22 ncsu edu)
 *          Vaclav Petras (wenzeslaus gmail com)
 *          Anna Petrasova (kratochanna gmail com)
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include "graster.hpp"

#include "pops/date.hpp"
#include "pops/raster.hpp"
#include "pops/simulation.hpp"
#include "pops/kernel.hpp"
#include "pops/treatments.hpp"
#include "pops/spread_rate.hpp"

#include "tcp_client.h"

extern "C" {
#include <grass/gis.h>
#include <grass/glocale.h>
#include <grass/vector.h>
#include <grass/raster.h>
}


#include <atomic>
#include <arpa/inet.h> //inet_addr
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>


#include <map>
#include <tuple>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>

#include <sys/stat.h>

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::round;
using std::isnan;
using std::atomic;
using std::thread;
using std::ref;

using namespace pops;

// TODO: for backwards compatibility, update eventually
typedef Simulation<Img, DImg> Sporulation;

#define DIM 1

// check if a file exists
inline bool file_exists(const char* name) {
    struct stat buffer;
    return (stat(name, &buffer) == 0);
}

inline void file_exists_or_fatal_error(struct Option* option) {
    if (option->answer && !file_exists(option->answer))
        G_fatal_error(_("Option %s: File %s does not exist"),
                      option->key, option->answer);
}

string generate_name(const string& basename, const Date& date)
{
    // counting on year being 4 digits
    auto year = G_double_to_basename_format(date.year(), 4, 0);
    auto month = G_double_to_basename_format(date.month(), 2, 0);
    auto day = G_double_to_basename_format(date.day(), 2, 0);
    auto sep = G_get_basename_separator();
    string name = basename + sep + year + "_" + month + "_" + day;
    return name;
}

inline TreatmentApplication treatment_app_enum_from_string(const string& text)
{
    std::map<string, TreatmentApplication> mapping{
        {"ratio_to_all", TreatmentApplication::Ratio},
        {"all_infected_in_cell", TreatmentApplication::AllInfectedInCell}
    };
    try {
        return mapping.at(text);
    }
    catch (const std::out_of_range&) {
        throw std::invalid_argument("treatment_application_enum_from_string:"
                                    " Invalid value '" + text +"' provided");
    }
}

inline Season seasonality_from_option(const Option* opt)
{
    return {std::atoi(opt->answers[0]), std::atoi(opt->answers[1])};
}


unsigned int get_num_answers(struct Option *opt)
{
    unsigned int i = 0;
    if (opt->answers)
        for (i = 0; opt->answers[i]; i++);
    return i;
}

void read_names(std::vector<string>& names, const char* filename)
{
    std::ifstream file(filename);
    string line;
    while (std::getline(file, line)) {
        names.push_back(line);
    }
}

/*!
 * Warns about depreciated option value
 *
 * It uses the answer member. If the answer is not set,
 * nothing is tested.
 *
 * \param opt Pointer to a valid option structure
 * \param depreciated Value which is depreciated
 * \param current Value which should be used instead
 */
void warn_about_depreciated_option_value(const Option* opt,
                                         const string& depreciated,
                                         const string& current)
{
    if (opt->answer && opt->answer == depreciated) {
        G_warning(_("The value <%s> for option %s is depreciated."
                    " Use value <%s> instead."),
                  opt->answer, opt->key, current.c_str());
    }
}

std::vector<double> weather_file_to_list(const string& filename)
{
    std::ifstream input(filename);
    std::vector<double> output;
    string line;
    while (std::getline(input, line))
    {
        double m, c;
        std::istringstream stream(line);
        stream >> m >> c;
        output.push_back(m * c);
    }
    return output;
}

void write_spread_rate(struct Option *opt, const std::vector<SpreadRate<Img>>& spread_rates,
                       unsigned num_years, int start_time) {
    FILE *fp = G_open_option_file(opt);
    fprintf(fp, "year,N,S,E,W\n");
    for (unsigned i = 0; i < num_years; i++) {
        double n, s, e, w;
        std::tie(n, s, e, w) = average_spread_rate(spread_rates, i);
        fprintf(fp, "%d,%.0f,%.0f,%.0f,%.0f\n", start_time + i,
                isnan(n) ? n : round(n), isnan(s) ? s : round(s),
                isnan(e) ? e : round(e), isnan(w) ? w : round(w));
    }
    G_close_option_file(fp);
}

void write_spread_rate(struct Option *opt, const SpreadRate<Img>& spread_rate,
                       unsigned num_years, int start_time) {
    FILE *fp = G_open_option_file(opt);
    fprintf(fp, "year,N,S,E,W\n");
    for (unsigned i = 0; i < num_years; i++) {
        double n, s, e, w;
        std::tie(n, s, e, w) = spread_rate.yearly_rate(i);
        fprintf(fp, "%d,%.0f,%.0f,%.0f,%.0f\n", start_time + i,
                isnan(n) ? n : round(n), isnan(s) ? s : round(s),
                isnan(e) ? e : round(e), isnan(w) ? w : round(w));
    }
    G_close_option_file(fp);
}

/** Checks if there are any susceptible hosts left */
bool all_infected(Img& susceptible)
{
    for (unsigned j = 0; j < susceptible.rows(); j++)
        for (unsigned k = 0; k < susceptible.cols(); k++)
            if (susceptible(j, k) > 0)
                return false;
    return true;
}

unsigned sum_of_infected(Img& infected)
{
    unsigned sum = 0;
    for (unsigned j = 0; j < infected.rows(); j++) {
        for (unsigned k = 0; k < infected.cols(); k++) {
            sum += infected(j, k);
        }
    }
    return sum;
}

unsigned select_run(std::vector<unsigned> stats) {
    // select index of median (or above median)
    auto stats2 = stats;
    auto median_it = stats2.begin() + stats2.size() / 2;
    std::nth_element(stats2.begin(), median_it, stats2.end());
    auto itOfMedian = std::find(stats.begin(), stats.end(), *median_it);
    return itOfMedian - stats.begin();
}

std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}


enum class SteeringCommand {None, Play, Pause, StepForward, StepBack, Stop, GoTo,
                            LoadData, ChangeName, SyncRuns};

 const char* print_command(const SteeringCommand &cmd)
{
    switch(cmd)
    {
    case SteeringCommand::None: return "None";
    case SteeringCommand::Play: return "Play";
    case SteeringCommand::Pause: return "Pause";
    case SteeringCommand::StepForward: return "StepForward";
    case SteeringCommand::StepBack: return "StepBack";
    case SteeringCommand::Stop: return "Stop";
    case SteeringCommand::GoTo: return "GoTo";
    case SteeringCommand::LoadData: return "LoadData";
    case SteeringCommand::ChangeName: return "ChangeName";
    case SteeringCommand::SyncRuns: return "SyncRuns";
    default: return "Undefined";
    }
}

class Steering {
private:
    std::queue<SteeringCommand> command_queue;
    std::mutex mutex;

public:
    string load_data;
    string basename;
    int goto_year;
    int treatment_year;

    inline void store(SteeringCommand cmd);
    inline SteeringCommand get();
};

void Steering::store(SteeringCommand cmd) {
    std::lock_guard<std::mutex> lk(mutex);
    command_queue.push(cmd);
}

SteeringCommand Steering::get() {
    std::lock_guard<std::mutex> lk(mutex);
    if (!command_queue.empty()) {
        SteeringCommand cmd = command_queue.front();
        command_queue.pop();
        return cmd;
    }
    else {
        return SteeringCommand::None;
    }
}
void steering_client(tcp_client &c, string ip_address, int port, Steering &steering)
{
    int rec_error;
    string received;
    bool break_flag = false;

    //connect to host
    c.conn(ip_address, port);

    while (true) {
        received = c.receive(200, rec_error);
        if (rec_error <= 0){
            cerr << "receive failed\n";
            c.close_socket();
            steering.store(SteeringCommand::Stop);
            break;
        }
        else {
            std::vector<std::string> received_vec = split(received, ';');
            for (int i = 0; i < received_vec.size(); i++) {
                std::string message = received_vec[i];
                if (message.substr(0, 3) == "cmd") {
                    string cmd = message.substr(4, message.length() - 4);
                    if (cmd == "play") {
                        steering.store(SteeringCommand::Play);
                    }
                    else if (cmd == "pause") {
                        steering.store(SteeringCommand::Pause);
                    }
                    else if (cmd == "stepf") {
                        steering.store(SteeringCommand::StepForward);
                    }
                    else if (cmd == "stepb") {
                        steering.store(SteeringCommand::StepBack);
                    }
                    else if (cmd == "stop") {
                        steering.store(SteeringCommand::Stop);
                        break_flag = true;
                        break;
                    }
                } else if (message.substr(0, 4) == "load") {
                    std::vector<std::string> received_load = split(message, ':');
                    steering.treatment_year = std::stoul(received_load[1]);
                    steering.load_data = received_load[2];
                    cout << "received load name: " << steering.load_data << endl;
                    steering.store(SteeringCommand::LoadData);
                } else if (message.substr(0, 4) == "name") {
                    string name = message.substr(5, message.length() - 5);
                    cout << "received base name: " << name << endl;
                    steering.basename = name;
                    steering.store(SteeringCommand::ChangeName);
                } else if (message.substr(0, 4) == "goto") {
                    string year = message.substr(5, message.length() - 5);
                    cout << "received goto year: " << year << endl;
                    steering.goto_year = std::stoi(year);;
                    steering.store(SteeringCommand::GoTo);
                } else if (message.substr(0, 4) == "sync") {
                    steering.store(SteeringCommand::SyncRuns);
                } else
                    cout << "X" << message << "X" << rec_error << endl;
            }
            if (break_flag) break;
        }
    }
}


struct PoPSOptions
{
    struct Option *host, *total_plants, *infected, *outside_spores;
    struct Option *moisture_coefficient_file, *temperature_coefficient_file;
    struct Option *weather_coefficient_file;
    struct Option *lethal_temperature, *lethal_temperature_months;
    struct Option *temperature_file;
    struct Option *start_time, *end_time, *seasonality;
    struct Option *step;
    struct Option *treatments;
    struct Option *treatment_year, *treatment_month;
    struct Option *treatment_app;
    struct Option *reproductive_rate;
    struct Option *natural_kernel, *natural_scale;
    struct Option *natural_direction, *natural_kappa;
    struct Option *anthro_kernel, *anthro_scale;
    struct Option *anthro_direction, *anthro_kappa;
    struct Option *percent_natural_dispersal;
    struct Option *infected_to_dead_rate, *first_year_to_die;
    struct Option *dead_series;
    struct Option *seed, *runs, *threads;
    struct Option *output, *output_series;
    struct Option *stddev, *stddev_series;
    struct Option *probability, *probability_series;
    struct Option *spread_rate_output;
    struct Option *ip_address, *port;
};

struct PoPSFlags
{
    struct Flag *mortality;
    struct Flag *generate_seed;
    struct Flag *series_as_single_run;
};


int main(int argc, char *argv[])
{
    PoPSOptions opt;
    PoPSFlags flg;

    G_gisinit(argv[0]);

    struct GModule *module = G_define_module();

    G_add_keyword(_("raster"));
    G_add_keyword(_("spread"));
    G_add_keyword(_("model"));
    G_add_keyword(_("disease"));
    G_add_keyword(_("pest"));
    module->description = _("A dynamic species distribution model for pest or "
                            "pathogen spread in forest or agricultural ecosystems");

    opt.host = G_define_standard_option(G_OPT_R_INPUT);
    opt.host->key = "host";
    opt.host->label = _("Input host raster map");
    opt.host->description = _("Number of hosts per cell.");
    opt.host->guisection = _("Input");

    opt.total_plants = G_define_standard_option(G_OPT_R_INPUT);
    opt.total_plants->key = "total_plants";
    opt.total_plants->label = _("Input raster map of total plants");
    opt.total_plants->description = _("Number of all plants per cell");
    opt.total_plants->guisection = _("Input");

    opt.infected = G_define_standard_option(G_OPT_R_INPUT);
    opt.infected->key = "infected";
    opt.infected->label = _("Input raster map of initial infection");
    opt.infected->description = _("Number of infected hosts per cell");
    opt.infected->guisection = _("Input");

    opt.output = G_define_standard_option(G_OPT_R_OUTPUT);
    opt.output->guisection = _("Output");
    opt.output->required = NO;

    opt.output_series = G_define_standard_option(G_OPT_R_BASENAME_OUTPUT);
    opt.output_series->key = "output_series";
    opt.output_series->description = _("Basename for output series");
    opt.output_series->required = NO;
    opt.output_series->guisection = _("Output");

    opt.stddev = G_define_standard_option(G_OPT_R_OUTPUT);
    opt.stddev->key = "stddev";
    opt.stddev->description = _("Standard deviations");
    opt.stddev->required = NO;
    opt.stddev->guisection = _("Output");

    opt.stddev_series = G_define_standard_option(G_OPT_R_BASENAME_OUTPUT);
    opt.stddev_series->key = "stddev_series";
    opt.stddev_series->description
            = _("Basename for output series of standard deviations");
    opt.stddev_series->required = NO;
    opt.stddev_series->guisection = _("Output");

    flg.series_as_single_run = G_define_flag();
    flg.series_as_single_run->key = 'l';
    flg.series_as_single_run->label =
            _("The output series as a single run only, not average");
    flg.series_as_single_run->description =
            _("The first run will be used for output instead of average");
    flg.series_as_single_run->guisection = _("Output");

    opt.probability = G_define_standard_option(G_OPT_R_OUTPUT);
    opt.probability->key = "probability";
    opt.probability->description = _("Infection probability (in percent)");
    opt.probability->required = NO;
    opt.probability->guisection = _("Output");

    opt.probability_series = G_define_standard_option(G_OPT_R_BASENAME_OUTPUT);
    opt.probability_series->key = "probability_series";
    opt.probability_series->description = _("Basename for output series of probabilities");
    opt.probability_series->required = NO;
    opt.probability_series->guisection = _("Output");

    opt.outside_spores = G_define_standard_option(G_OPT_V_OUTPUT);
    opt.outside_spores->key = "outside_spores";
    opt.outside_spores->description = _("Output vector map of spores or pest units outside of modeled area");
    opt.outside_spores->required = NO;
    opt.outside_spores->guisection = _("Output");

    opt.spread_rate_output = G_define_standard_option(G_OPT_F_OUTPUT);
    opt.spread_rate_output->key = "spread_rate_output";
    opt.spread_rate_output->description =
        _("Output CSV file containg yearly spread rate in N, S, E, W directions");
    opt.spread_rate_output->required = NO;
    opt.spread_rate_output->guisection = _("Output");

    opt.treatments = G_define_standard_option(G_OPT_R_INPUT);
    opt.treatments->key = "treatments";
    opt.treatments->multiple = YES;
    opt.treatments->description = _("Raster map(s) of treatments (treated 1, otherwise 0)");
    opt.treatments->required = NO;
    opt.treatments->guisection = _("Treatments");

    opt.treatment_year = G_define_option();
    opt.treatment_year->key = "treatment_year";
    opt.treatment_year->type = TYPE_INTEGER;
    opt.treatment_year->multiple = YES;
    opt.treatment_year->description = _("Years when treatment rasters are applied");
    opt.treatment_year->required = NO;
    opt.treatment_year->guisection = _("Treatments");

    opt.treatment_month = G_define_option();
    opt.treatment_month->type = TYPE_INTEGER;
    opt.treatment_month->key = "treatment_month";
    opt.treatment_month->label =
            _("Month when the treatment is applied");
    opt.treatment_month->description =
            _("Treatment is applied at the beginning of the month");
    // TODO: implement this as multiple or a season
    opt.treatment_month->required = NO;
    opt.treatment_month->guisection = _("Treatments");

    opt.treatment_app = G_define_option();
    opt.treatment_app->key = "treatment_application";
    opt.treatment_app->type = TYPE_STRING;
    opt.treatment_app->multiple = NO;
    opt.treatment_app->description = _("Type of treatmet application");
    opt.treatment_app->options = "ratio_to_all,all_infected_in_cell";
    opt.treatment_app->required = NO;
    opt.treatment_app->answer = const_cast<char*>("ratio_to_all");
    opt.treatment_app->guisection = _("Treatments");

    opt.moisture_coefficient_file = G_define_standard_option(G_OPT_F_INPUT);
    opt.moisture_coefficient_file->key = "moisture_coefficient_file";
    opt.moisture_coefficient_file->label =
            _("Input file with one moisture coefficient map name per line");
    opt.moisture_coefficient_file->description =
            _("Moisture coefficient");
    opt.moisture_coefficient_file->required = NO;
    opt.moisture_coefficient_file->guisection = _("Weather");

    opt.temperature_coefficient_file = G_define_standard_option(G_OPT_F_INPUT);
    opt.temperature_coefficient_file->key = "temperature_coefficient_file";
    opt.temperature_coefficient_file->label =
            _("Input file with one temperature coefficient map name per line");
    opt.temperature_coefficient_file->description =
            _("Temperature coefficient");
    opt.temperature_coefficient_file->required = NO;
    opt.temperature_coefficient_file->guisection = _("Weather");

    opt.weather_coefficient_file = G_define_standard_option(G_OPT_F_INPUT);
    opt.weather_coefficient_file->key = "weather_coefficient_file";
    opt.weather_coefficient_file->label =
        _("Input file with one weather coefficient map name per line");
    opt.weather_coefficient_file->description =
        _("Weather coefficient");
    opt.weather_coefficient_file->required = NO;
    opt.weather_coefficient_file->guisection = _("Weather");

    opt.lethal_temperature = G_define_option();
    opt.lethal_temperature->type = TYPE_DOUBLE;
    opt.lethal_temperature->key = "lethal_temperature";
    opt.lethal_temperature->label =
            _("Temperature at which the pest or pathogen dies");
    opt.lethal_temperature->description =
            _("The temerature unit must be the same as for the"
              "temerature raster map (typically degrees of Celsius)");
    opt.lethal_temperature->required = NO;
    opt.lethal_temperature->multiple = NO;
    opt.lethal_temperature->guisection = _("Weather");

    opt.lethal_temperature_months = G_define_option();
    opt.lethal_temperature_months->type = TYPE_INTEGER;
    opt.lethal_temperature_months->key = "lethal_month";
    opt.lethal_temperature_months->label =
            _("Month when the pest or patogen dies due to low temperature");
    opt.lethal_temperature_months->description =
            _("The temperature unit must be the same as for the"
              "temperature raster map (typically degrees of Celsius)");
    // TODO: implement this as multiple
    opt.lethal_temperature_months->required = NO;
    opt.lethal_temperature_months->guisection = _("Weather");

    // TODO: rename coefs in interface and improve their descs
    opt.temperature_file = G_define_standard_option(G_OPT_F_INPUT);
    opt.temperature_file->key = "temperature_file";
    opt.temperature_file->label =
            _("Input file with one temperature raster map name per line");
    opt.temperature_file->description =
            _("The temperature should be in actual temperature units (typically degrees of Celsius)");
    opt.temperature_file->required = NO;
    opt.temperature_file->guisection = _("Weather");

    opt.start_time = G_define_option();
    opt.start_time->type = TYPE_INTEGER;
    opt.start_time->key = "start_time";
    opt.start_time->label = _("Start year of the simulation");
    opt.start_time->description = _("The first day of the year will be used");
    opt.start_time->required = YES;
    opt.start_time->guisection = _("Time");

    opt.end_time = G_define_option();
    opt.end_time->type = TYPE_INTEGER;
    opt.end_time->key = "end_time";
    opt.end_time->label = _("End year of the simulation");
    opt.end_time->description = _("The last day of the year will be used");
    opt.end_time->required = YES;
    opt.end_time->guisection = _("Time");

    opt.seasonality = G_define_option();
    opt.seasonality->type = TYPE_STRING;
    opt.seasonality->key = "seasonality";
    opt.seasonality->label = _("Seasonal spread (from,to)");
    opt.seasonality->description =
            _("Spread limited to certain months (season), for example"
              " 5,9 for spread starting at the beginning of May and"
              " ending at the end of September");
    opt.seasonality->key_desc = "from,to";
    //opt.seasonality->options = "1-12";
    opt.seasonality->answer = const_cast<char*>("1,12");
    opt.seasonality->required = YES;
    opt.seasonality->multiple = NO;
    opt.seasonality->guisection = _("Time");

    opt.step = G_define_option();
    opt.step->type = TYPE_STRING;
    opt.step->key = "step";
    opt.step->label = _("Simulation step");
    opt.step->description = _("How often the simulation computes new step");
    opt.step->options = "week,month";
    opt.step->descriptions = _("week;Compute next simulation step each week;month;Compute next simulation step each month");
    opt.step->required = YES;
    opt.step->guisection = _("Time");

    opt.reproductive_rate = G_define_option();
    opt.reproductive_rate->type = TYPE_DOUBLE;
    opt.reproductive_rate->key = "reproductive_rate";
    opt.reproductive_rate->label = _("Number of spores or pest units produced by a single host");
    opt.reproductive_rate->description = _("Number of spores or pest units produced by a single host under optimal weather conditions");
    opt.reproductive_rate->answer = const_cast<char*>("4.4");
    opt.reproductive_rate->guisection = _("Dispersal");

    opt.natural_kernel = G_define_option();
    opt.natural_kernel->type = TYPE_STRING;
    opt.natural_kernel->key = "natural_dispersal_kernel";
    opt.natural_kernel->label = _("Natural dispersal kernel type");
    opt.natural_kernel->answer = const_cast<char*>("cauchy");
    opt.natural_kernel->options = "cauchy,exponential";
    opt.natural_kernel->guisection = _("Dispersal");

    opt.natural_scale = G_define_option();
    opt.natural_scale->type = TYPE_DOUBLE;
    opt.natural_scale->key = "natural_distance";
    opt.natural_scale->label =
            _("Distance parameter for natural dispersal kernel");
    opt.natural_scale->guisection = _("Dispersal");

    opt.natural_direction = G_define_option();
    opt.natural_direction->type = TYPE_STRING;
    opt.natural_direction->key = "natural_direction";
    opt.natural_direction->label =
            _("Direction of natural dispersal kernel");
    opt.natural_direction->description =
            _("Typically prevailing wind direction;"
              " none means that there is no directionality or no wind");
    opt.natural_direction->options = "N,NE,E,SE,S,SW,W,NW,NONE,none";
    opt.natural_direction->required = YES;
    opt.natural_direction->answer = const_cast<char*>("none");
    opt.natural_direction->guisection = _("Dispersal");

    opt.natural_kappa = G_define_option();
    opt.natural_kappa->type = TYPE_DOUBLE;
    opt.natural_kappa->key = "natural_direction_strength";
    opt.natural_kappa->label =
            _("Strength of direction of natural dispersal kernel");
    opt.natural_kappa->description =
            _("The kappa parameter of von Mises distribution"
              " (concentration);"
              " typically the strength of the wind direction");
    opt.natural_kappa->required = YES;
    opt.natural_kappa->guisection = _("Dispersal");

    opt.anthro_kernel = G_define_option();
    opt.anthro_kernel->type = TYPE_STRING;
    opt.anthro_kernel->key = "anthropogenic_dispersal_kernel";
    opt.anthro_kernel->label = _("Anthropogenic dispersal kernel type");
    opt.anthro_kernel->options = "cauchy,exponential";
    opt.anthro_kernel->guisection = _("Dispersal");

    opt.anthro_scale = G_define_option();
    opt.anthro_scale->type = TYPE_DOUBLE;
    opt.anthro_scale->key = "anthropogenic_distance";
    opt.anthro_scale->label =
            _("Distance parameter for anthropogenic dispersal kernel");
    opt.anthro_scale->guisection = _("Dispersal");

    opt.anthro_direction = G_define_option();
    opt.anthro_direction->type = TYPE_STRING;
    opt.anthro_direction->key = "anthropogenic_direction";
    opt.anthro_direction->label =
            _("Direction of anthropogenic dispersal kernel");
    opt.anthro_direction->description =
            _("Value none means that there is no directionality");
    opt.anthro_direction->options = "N,NE,E,SE,S,SW,W,NW,NONE,none";
    opt.anthro_direction->required = YES;
    opt.anthro_direction->answer = const_cast<char*>("none");
    opt.anthro_direction->guisection = _("Dispersal");

    opt.anthro_kappa = G_define_option();
    opt.anthro_kappa->type = TYPE_DOUBLE;
    opt.anthro_kappa->key = "anthropogenic_direction_strength";
    opt.anthro_kappa->label =
            _("Strength of direction of anthropogenic dispersal kernel");
    opt.anthro_kappa->description =
            _("The kappa parameter of von Mises distribution"
              " (concentration);"
              " typically the strength of the wind direction");
    opt.anthro_kappa->guisection = _("Dispersal");

    opt.percent_natural_dispersal = G_define_option();
    opt.percent_natural_dispersal->type = TYPE_DOUBLE;
    opt.percent_natural_dispersal->key = "percent_natural_dispersal";
    opt.percent_natural_dispersal->label =
            _("Percentage of natural dispersal");
    opt.percent_natural_dispersal->description =
            _("How often is the natural dispersal kernel used versus"
              " the anthropogenic dispersal kernel");
    opt.percent_natural_dispersal->options = "0-1";
    opt.percent_natural_dispersal->guisection = _("Dispersal");

    opt.infected_to_dead_rate = G_define_option();
    opt.infected_to_dead_rate->type = TYPE_DOUBLE;
    opt.infected_to_dead_rate->key = "mortality_rate";
    opt.infected_to_dead_rate->label =
            _("Mortality rate of infected hosts");
    opt.infected_to_dead_rate->description =
            _("Percentage of infected hosts that die in a given year"
              " (hosts are removed from the infected pool)");
    opt.infected_to_dead_rate->options = "0-1";
    opt.infected_to_dead_rate->guisection = _("Mortality");

    opt.first_year_to_die = G_define_option();
    opt.first_year_to_die->type = TYPE_INTEGER;
    opt.first_year_to_die->key = "mortality_time_lag";
    opt.first_year_to_die->label =
            _("Time lag from infection until mortality can occur in years");
    opt.first_year_to_die->description =
            _("How many years it takes for an infected host to die"
              " (value 1 for hosts dying at the end of the first year)");
    opt.first_year_to_die->guisection = _("Mortality");

    opt.dead_series = G_define_standard_option(G_OPT_R_BASENAME_OUTPUT);
    opt.dead_series->key = "mortality_series";
    opt.dead_series->label =
            _("Basename for series of number of dead hosts");
    opt.dead_series->description =
            _("Basename for output series of number of dead hosts"
              " (requires mortality to be activated)");
    opt.dead_series->required = NO;
    opt.dead_series->guisection = _("Mortality");

    flg.mortality = G_define_flag();
    flg.mortality->key = 'm';
    flg.mortality->label =
            _("Apply mortality");
    flg.mortality->description =
            _("After certain number of years, start removing dead hosts"
              " from the infected pool with a given rate");
    flg.mortality->guisection = _("Mortality");

    opt.seed = G_define_option();
    opt.seed->key = "random_seed";
    opt.seed->type = TYPE_INTEGER;
    opt.seed->required = NO;
    opt.seed->label = _("Seed for random number generator");
    opt.seed->description =
            _("The same seed can be used to obtain same results"
              " or random seed can be generated by other means.");
    opt.seed->guisection = _("Randomness");

    flg.generate_seed = G_define_flag();
    flg.generate_seed->key = 's';
    flg.generate_seed->label =
            _("Generate random seed (result is non-deterministic)");
    flg.generate_seed->description =
            _("Automatically generates random seed for random number"
              " generator (use when you don't want to provide the seed option)");
    flg.generate_seed->guisection = _("Randomness");

    opt.runs = G_define_option();
    opt.runs->key = "runs";
    opt.runs->type = TYPE_INTEGER;
    opt.runs->required = NO;
    opt.runs->label = _("Number of simulation runs");
    opt.runs->description =
            _("The individual runs will obtain different seeds"
              " and will be averaged for the output");
    opt.runs->guisection = _("Randomness");

    opt.threads = G_define_option();
    opt.threads->key = "nprocs";
    opt.threads->type = TYPE_INTEGER;
    opt.threads->required = NO;
    opt.threads->description =
            _("Number of threads for parallel computing");
    opt.threads->options = "1-";
    opt.threads->guisection = _("Randomness");

    opt.ip_address = G_define_option();
    opt.ip_address->key = "ip_address";
    opt.ip_address->type = TYPE_STRING;
    opt.ip_address->required = NO;
    opt.ip_address->description = _("IP address of steering server");
    opt.ip_address->guisection = _("Steering");

    opt.port = G_define_option();
    opt.port->key = "port";
    opt.port->type = TYPE_INTEGER;
    opt.port->required = NO;
    opt.port->description = _("Port of steering server");
    opt.port->guisection = _("Steering");

    G_option_required(opt.output, opt.output_series, opt.probability, opt.probability_series,
                      opt.outside_spores, NULL);

    G_option_exclusive(opt.seed, flg.generate_seed, NULL);
    G_option_required(opt.seed, flg.generate_seed, NULL);
    G_option_collective(opt.ip_address, opt.port, NULL);

    // weather
    G_option_collective(opt.moisture_coefficient_file, opt.temperature_coefficient_file, NULL);
    G_option_exclusive(opt.moisture_coefficient_file, opt.weather_coefficient_file, NULL);
    G_option_exclusive(opt.temperature_coefficient_file, opt.weather_coefficient_file, NULL);

    // mortality
    // flag and rate required always
    // for simplicity of the code outputs allowed only with output
    // for single run (avgs from runs likely not needed)
    G_option_requires(flg.mortality, opt.infected_to_dead_rate, NULL);
    G_option_requires(opt.first_year_to_die, flg.mortality, NULL);
    G_option_requires_all(opt.dead_series, flg.mortality,
                          flg.series_as_single_run, NULL);
    // TODO: requires_all does not understand the default?
    // treatment_app needs to be removed from here and check separately
    G_option_requires_all(opt.treatments,
                          opt.treatment_year,
                          opt.treatment_month,
                          opt.treatment_app,
                          NULL);

    if (G_parser(argc, argv))
        exit(EXIT_FAILURE);

    unsigned num_runs = 1;
    if (opt.runs->answer)
        num_runs = std::stoul(opt.runs->answer);

    unsigned threads = 1;
    if (opt.threads->answer)
        threads = std::stoul(opt.threads->answer);

    // check for file existence
    file_exists_or_fatal_error(opt.moisture_coefficient_file);
    file_exists_or_fatal_error(opt.temperature_coefficient_file);
    file_exists_or_fatal_error(opt.weather_coefficient_file);

    // get current computational region (for rows, cols and resolution)
    struct Cell_head window;
    G_get_window(&window);

    // Seasonality: Do you want the spread to be limited to certain months?
    if (!opt.seasonality->answer || opt.seasonality->answer[0] == '\0')
        G_fatal_error(_("The option %s cannot be empty"),
                      opt.seasonality->key);
    Season season = seasonality_from_option(opt.seasonality);

    // set the spore rate
    double spore_rate = std::stod(opt.reproductive_rate->answer);

    // TODO: how to support DispersalKernelType::None for natural_kernel?
    // perhaps the long-short should take the type instead of bool,
    // then T is anything else than None
    // TODO: should all kernels support None?
    DispersalKernelType natural_kernel_type =
            kernel_type_from_string(opt.natural_kernel->answer);
    double natural_scale = std::stod(opt.natural_scale->answer);
    Direction natural_direction = direction_from_string(opt.natural_direction->answer);
    double natural_kappa = std::stod(opt.natural_kappa->answer);

    DispersalKernelType anthro_kernel_type =
            kernel_type_from_string(opt.anthro_kernel->answer);
    bool use_long_kernel = false;
    if (anthro_kernel_type != DispersalKernelType::None)
        use_long_kernel = true;

    double anthro_scale = 0;
    if (use_long_kernel && !opt.anthro_scale->answer)
        G_fatal_error(_("The option %s is required for %s=%s"),
                      opt.anthro_scale->key, opt.anthro_kernel->key,
                      opt.anthro_kernel->answer);
    else if (opt.anthro_scale->answer)
        anthro_scale = std::stod(opt.anthro_scale->answer);

    // we allow none and an empty string
    Direction anthro_direction = direction_from_string(opt.anthro_direction->answer);

    double anthro_kappa = 0;
    if (use_long_kernel && !opt.anthro_kappa->answer)
        G_fatal_error(_("The option %s is required for %s=%s"),
                      opt.anthro_kappa->key, opt.anthro_kernel->key,
                      opt.anthro_kernel->answer);
    else if (opt.anthro_kappa->answer)
        anthro_kappa= std::stod(opt.anthro_kappa->answer);

    double gamma = 0.0;
    if (use_long_kernel &&
            !opt.percent_natural_dispersal->answer)
        G_fatal_error(_("The option %s is required for %s=%s"),
                      opt.percent_natural_dispersal->key, opt.natural_kernel->key,
                      opt.natural_kernel->answer);
    else if (opt.percent_natural_dispersal->answer)
        gamma = std::stod(opt.percent_natural_dispersal->answer);

    // warn about limits to backwards compatibility
    // "none" is consistent with other GRASS GIS modules
    warn_about_depreciated_option_value(
                opt.natural_direction, "NONE", "none");
    warn_about_depreciated_option_value(
                opt.anthro_kernel, "NONE", "none");
    warn_about_depreciated_option_value(
                opt.anthro_direction, "NONE", "none");

    RadialDispersalKernel short_radial_kernel(
                window.ew_res, window.ns_res, natural_kernel_type,
                natural_scale, natural_direction, natural_kappa);
    RadialDispersalKernel long_radial_kernel(
                window.ew_res, window.ns_res, anthro_kernel_type,
                anthro_scale, anthro_direction, anthro_kappa);
    UniformDispersalKernel uniform_kernel(window.rows, window.cols);
    SwitchDispersalKernel short_selectable_kernel(
                natural_kernel_type,
                short_radial_kernel, uniform_kernel);
    SwitchDispersalKernel long_selectable_kernel(
                natural_kernel_type,
                long_radial_kernel, uniform_kernel);
    // each run has its own copy, so a kernel can have a state
    std::vector<DispersalKernel> kernels;
    kernels.reserve(num_runs);
    for (unsigned i = 0; i < num_runs; ++i) {
        // each (sub)kernel is copied to the main kernel
        kernels.emplace_back(short_selectable_kernel,
                             long_selectable_kernel,
                             use_long_kernel,
                             gamma);
    }

    // initialize the start Date and end Date object
    // options for times are required ints
    int start_time = std::stoi(opt.start_time->answer);
    int end_time = std::stoi(opt.end_time->answer);
    if (start_time > end_time) {
        G_fatal_error(_("Start date must precede the end date"));
    }

    Date dd_start(start_time, 01, 01);
    Date dd_end(end_time, 12, 31);
    // difference in years (in dates) but including both years
    unsigned num_years = dd_end.year() - dd_start.year() + 1;

    string step_type = opt.step->answer;

    // mortality
    bool mortality = false;
    unsigned first_year_to_die = 1;  // starts at 1 (same as the opt)
    double infected_to_dead_rate = 0.0;
    if (flg.mortality->answer) {
        mortality = true;
        if (opt.first_year_to_die->answer) {
            first_year_to_die = std::stoi(opt.first_year_to_die->answer);
            if (first_year_to_die > num_years) {
                G_fatal_error(
                            _("%s is too large (%d). It must be smaller or "
                              " equal than number of simulation years (%d)."),
                            opt.first_year_to_die->key,
                            first_year_to_die, num_years);
            }
        }
        if (opt.infected_to_dead_rate->answer)
            infected_to_dead_rate = std::stod(opt.infected_to_dead_rate->answer);
    }

    unsigned seed_value;
    if (opt.seed->answer) {
        seed_value = std::stoul(opt.seed->answer);
        G_verbose_message(_("Read random seed from %s option: %ud"),
                          opt.seed->key, seed_value);
    } else {
        // flag or option is required, so no check needed
        // getting random seed using GRASS library
        // std::random_device is deterministic in MinGW (#338)
        seed_value = G_srand48_auto();
        G_verbose_message(_("Generated random seed (-%c): %ud"),
                          flg.generate_seed->key, seed_value);
    }

    // read the suspectible UMCA raster image
    Img species_rast = raster_from_grass_integer(opt.host->answer);

    // read the living trees raster image
    Img lvtree_rast = raster_from_grass_integer(opt.total_plants->answer);

    // read the initial infected oaks image
    Img I_species_rast = raster_from_grass_integer(opt.infected->answer);

    // create the initial suspectible oaks image
    Img S_species_rast = species_rast - I_species_rast;

    // save for the start checkpoint
    const Img I_species_rast_start = I_species_rast;
    const Img S_species_rast_start = S_species_rast;

    // SOD-immune trees image
    //Img SOD_rast = umca_rast + oaks_rast;
    //Img IMM_rast = lvtree_rast - SOD_rast;

    std::vector<string> moisture_names;
    std::vector<string> temperature_names;
    std::vector<string> weather_names;
    bool weather = false;
    bool moisture_temperature = false;

    if (opt.moisture_coefficient_file->answer && opt.temperature_coefficient_file->answer) {
        read_names(moisture_names, opt.moisture_coefficient_file->answer);
        read_names(temperature_names, opt.temperature_coefficient_file->answer);
        moisture_temperature = true;
    }
    if (opt.weather_coefficient_file->answer) {
        read_names(weather_names, opt.weather_coefficient_file->answer);
        weather = true;
    }

    double use_lethal_temperature = false;
    double lethal_temperature_value;
    int lethal_temperature_month = 0;  // invalid value for month
    std::vector<string> actual_temperature_names;
    std::vector<DImg> actual_temperatures;
    if (opt.lethal_temperature->answer)
        lethal_temperature_value = std::stod(opt.lethal_temperature->answer);
    if (opt.lethal_temperature_months->answer)
        lethal_temperature_month = std::stod(opt.lethal_temperature_months->answer);
    if (opt.temperature_file->answer) {
        file_exists_or_fatal_error(opt.temperature_file);
        read_names(actual_temperature_names, opt.temperature_file->answer);
        for (string name : actual_temperature_names) {
            actual_temperatures.push_back(raster_from_grass_float(name));
        }
        use_lethal_temperature = true;
    }

    // TODO: limit this by the actual max steps
    // weeks are currently the worst case
    const unsigned max_weeks_in_year = 53;
    std::vector<DImg> weather_coefficients;
    if (weather || moisture_temperature)
        weather_coefficients.resize(max_weeks_in_year);

    // treatments
    int treatment_month = 0;  // invalid value for month
    if (get_num_answers(opt.treatments) != get_num_answers(opt.treatment_year)){
        G_fatal_error(_("%s= and %s= must have the same number of values"), opt.treatments->key, opt.treatment_year->key);}
    // the default here should be never used
    TreatmentApplication treatment_app = TreatmentApplication::Ratio;
    if (opt.treatment_app->answer)
        treatment_app = treatment_app_enum_from_string(opt.treatment_app->answer);
    Treatments<Img, DImg> treatments(treatment_app);
    bool use_treatments = false;
    if (opt.treatments->answers) {
        for (int i_t = 0; opt.treatment_year->answers[i_t]; i_t++) {
            DImg tr = raster_from_grass_float(opt.treatments->answers[i_t]);
            treatments.add_treatment(std::stoul(opt.treatment_year->answers[i_t]), tr);
            use_treatments = true;
        }
    }
    if (opt.treatment_month->answer)
        treatment_month = std::stod(opt.treatment_month->answer);

    // build the Sporulation object
    std::vector<Sporulation> sporulations;
    std::vector<Img> sus_species_rasts(num_runs, S_species_rast);
    std::vector<Img> inf_species_rasts(num_runs, I_species_rast);

    // infected cohort for each year (index is cohort age)
    // age starts with 0 (in year 1), 0 is oldest
    std::vector<std::vector<Img> > inf_species_cohort_rasts(
                num_runs, std::vector<Img>(num_years, Img(S_species_rast, 0)));

    // we are using only the first dead img for visualization, but for
    // parallelization we need all allocated anyway
    std::vector<Img> dead_in_current_year(num_runs, Img(S_species_rast, 0));
    // dead trees accumulated over years
    // TODO: allow only when series as single run
    Img accumulated_dead(Img(S_species_rast, 0));

    sporulations.reserve(num_runs);
    for (unsigned i = 0; i < num_runs; ++i)
        sporulations.emplace_back(seed_value++, I_species_rast);
    std::vector<std::vector<std::tuple<int, int> > > outside_spores(num_runs);

    // spread rate initialization
    std::vector<SpreadRate<Img>> spread_rates(num_runs,
                                              SpreadRate<Img>(I_species_rast, window.ew_res, window.ns_res, num_years));

    std::vector<unsigned> unresolved_steps;
    std::vector<Date> unresolved_dates;
    unresolved_steps.reserve(max_weeks_in_year);
    unresolved_dates.reserve(max_weeks_in_year);

    // check if steering is on
    bool steering = false;
    if (opt.ip_address->answer)
        steering = true;

    // setup steering variables
    Date dd_current(dd_start);
    Date dd_current_end(dd_end);
    if (steering)
        dd_current_end = dd_start;
    Date dd_current_last_day = step_type == "month" ? dd_current.get_last_day_of_month() : dd_current.get_last_day_of_week();
    Steering steering_obj;
    // don't process outputs at the end of year
    // when we went there using checkpointing
    bool after_loading_checkpoint = false;

    // syncing runs
    bool sync = false;

    // setup client
    tcp_client c;
    thread client_thread;
    string ip = steering ? string(opt.ip_address->answer) : "";
    int port = steering ? atoi(opt.port->answer): 0;
    if (steering) {
        use_treatments = true;
        client_thread = thread(steering_client, ref(c), ip, port, ref(steering_obj));
    }

    // simulation years are closed interval
    // size 4 for 2016 to 2018 - 0: beginning 2016, 1: end 2016, 2: end 2017, 3: end 2018
    auto num_checkpoints = dd_end.year() - dd_start.year() + 2;
    std::vector<std::vector<Img>> sus_checkpoint(
                num_checkpoints, std::vector<Img>(num_runs, Img(S_species_rast)));
    std::vector<std::vector<Img>> inf_checkpoint(
                num_checkpoints, std::vector<Img>(num_runs, Img(S_species_rast)));
    std::vector<int> step_checkpoint(num_checkpoints);
    std::vector<Date> date_checkpoint(num_checkpoints, dd_start);
    int last_checkpoint = 0;
    for (unsigned run = 0; run < num_runs; run++) {
        sus_checkpoint[last_checkpoint][run] = S_species_rast_start;
        inf_checkpoint[last_checkpoint][run] = I_species_rast_start;
        step_checkpoint[last_checkpoint] = 0;
        date_checkpoint[last_checkpoint] = dd_start;
    }
    // main simulation loop (weekly or monthly steps)
    int current_step = 0;
    while (true) {
        SteeringCommand cmd = SteeringCommand::None;
        {
            cmd = steering_obj.get();
        }
        if (cmd != SteeringCommand::None){
            G_verbose_message("Code: %s", print_command(cmd));
        }
        if (cmd == SteeringCommand::Play) { // play from
            dd_current_end = dd_end;
        }
        else if (cmd == SteeringCommand::Pause) { // pause
            dd_current_end = dd_current;
        }
        else if (cmd == SteeringCommand::StepForward) { // 1 step forward
            dd_current_end = dd_current.get_next_year_end();
            if (dd_current_end > dd_end)
                dd_current_end = dd_end;
        }
        else if (cmd == SteeringCommand::StepBack) { // 1 step back
            if (last_checkpoint - 1 >= 0) {
                --last_checkpoint;
                dd_current_end = date_checkpoint[last_checkpoint];
                dd_current = date_checkpoint[last_checkpoint];
                for (unsigned run = 0; run < num_runs; run++) {
                    sus_species_rasts[run] = sus_checkpoint[last_checkpoint][run];
                    inf_species_rasts[run] = inf_checkpoint[last_checkpoint][run];
                    current_step = step_checkpoint[last_checkpoint];
                    unresolved_steps.clear();
                }
                G_verbose_message("Going back to date: %d-%d-%d", dd_current.year(), dd_current.month(), dd_current.day());
                after_loading_checkpoint = true;
            }
            // we are at the end of year, but we have already computed
            // the simulation for this year
            //            continue;
        }
        else if (cmd == SteeringCommand::Stop) { // complete stop
            break;
        }
        else if (cmd == SteeringCommand::LoadData) { // load treatments
            G_verbose_message("Loading treatments: %s", steering_obj.load_data.c_str());
            treatments.clear_after_year(steering_obj.treatment_year);
            DImg tr = raster_from_grass_float(steering_obj.load_data);
            treatments.add_treatment(steering_obj.treatment_year, tr);
        }
        else if (cmd == SteeringCommand::ChangeName) { // base name changed
            G_verbose_message("Base name: %s", steering_obj.basename.c_str());
        }
        else if (cmd == SteeringCommand::GoTo) { // go to specific checkpoint
            G_verbose_message("Go to checkpoint: %d", steering_obj.goto_year);
            unsigned goto_checkpoint = steering_obj.goto_year;
            if (goto_checkpoint < 0 || goto_checkpoint >= num_checkpoints) {/* do nothing */}
            else if (goto_checkpoint <= last_checkpoint) {
                // go back
                dd_current = date_checkpoint[goto_checkpoint];
                dd_current_end = date_checkpoint[goto_checkpoint];
                unresolved_steps.clear();
                G_verbose_message("Going to date: %d-%d-%d", dd_current.year(), dd_current.month(), dd_current.day());
                for (unsigned run = 0; run < num_runs; run++) {
                    sus_species_rasts[run] = sus_checkpoint[goto_checkpoint][run];
                    inf_species_rasts[run] = inf_checkpoint[goto_checkpoint][run];
                    current_step = step_checkpoint[goto_checkpoint];
                }
                after_loading_checkpoint = true;
            }
            else {
                // go forward
                dd_current_end = Date(steering_obj.goto_year + dd_start.year() - 1, 12, 31);
            }
        }
        else if (cmd == SteeringCommand::SyncRuns) {
            // compute sums of infected for each run
            /* This doesn't work well, because the exported ones are always first
               since we can't know the average run in advance. When going back the syncing
               would sync it to different year than was exported
               We would have to decide based on first year (after syncing or starting)
               and the use that selection till next syncing
            */
/*
            std::vector<unsigned> sums;
            for (unsigned run = 0; run < num_runs; run++) {
                sums.push_back(sum_of_infected(inf_species_rasts[run]));
            }
            unsigned selected_run = select_run(sums);
*/
            sync = true;
        }

        string last_name = "";
        if (dd_current_end > dd_start && dd_current <= dd_current_end) {
            unresolved_steps.push_back(current_step);
            unresolved_dates.push_back(dd_current);
            dd_current_last_day = step_type == "month" ? dd_current.get_last_day_of_month() : dd_current.get_last_day_of_week();

            // if all the oaks are infected, then exit
            if (all_infected(S_species_rast)) {
                G_warning("All suspectible oaks are infected!");
                break;
            }

            // check whether the spore occurs in the month
            // At the end of the year, run simulation for all unresolved
            // steps in one chunk.
            if ((step_type == "month" ? dd_current.is_last_month_of_year() : dd_current.is_last_week_of_year()) && !after_loading_checkpoint) {
                if (!unresolved_steps.empty()) {

                    // to avoid problem with Jan 1 of the following year
                    // we explicitely check if we are in a valid year range
                    // TODO: will this ever happen here?
                    unsigned simulation_year = dd_current.year() - dd_start.year();
                    if (use_lethal_temperature
                            && simulation_year >= actual_temperatures.size())
                        G_fatal_error(_("Not enough temperatures"));

                    unsigned step_in_chunk = 0;
                    // get weather for all the steps in chunk
                    for (auto step : unresolved_steps) {
                        if (moisture_temperature) {
                            DImg moisture(raster_from_grass_float(moisture_names[step]));
                            DImg temperature(raster_from_grass_float(temperature_names[step]));
                            weather_coefficients[step_in_chunk] = moisture * temperature;
                        }
                        else if (weather)
                            weather_coefficients[step_in_chunk] = raster_from_grass_float(weather_names[step]);
                        ++step_in_chunk;
                    }

                    // stochastic simulation runs
                    #pragma omp parallel for num_threads(threads)
                    for (unsigned run = 0; run < num_runs; run++) {
                        bool lethality_done_this_year = false;
                        bool treatments_done_this_year = false;
                        // actual runs of the simulation for each step
                        for (unsigned step = 0; step < unresolved_steps.size(); ++step) {
                            Date date = unresolved_dates[step];
                            // removal of dispersers
                            if (use_lethal_temperature && !lethality_done_this_year
                                    && date.month() == lethal_temperature_month) {
                                sporulations[run].remove(inf_species_rasts[run],
                                                         sus_species_rasts[run],
                                                         actual_temperatures[simulation_year],
                                                         lethal_temperature_value);
                                lethality_done_this_year = true;
                            }
                            if (use_treatments && !treatments_done_this_year
                                    && date.month() == treatment_month) {
                                treatments.apply_treatment_host(date.year(), inf_species_rasts[run], sus_species_rasts[run]);
                                if (mortality) {
                                    // same conditions as the mortality code below
                                    // TODO: make the mortality timing available as a separate function in the library
                                    // or simply go over all valid cohorts
                                    unsigned simulation_year = dd_current.year() - dd_start.year();
                                    if (simulation_year >= first_year_to_die - 1) {
                                        auto max_index = simulation_year - (first_year_to_die - 1);
                                        for (unsigned age = 0; age <= max_index; age++) {
                                            if (use_treatments)
                                                treatments.apply_treatment_infected(dd_current.year(), inf_species_cohort_rasts[run][age]);
                                        }
                                    }
                                }
                                treatments_done_this_year = true;
                            }
                            if (!season.month_in_season(date.month()))
                                continue;
                            sporulations[run].generate(inf_species_rasts[run],
                                                       weather || moisture_temperature,
                                                       weather_coefficients[step],
                                                       spore_rate);

                            auto current_age = dd_current.year() - dd_start.year();
                            sporulations[run].disperse(sus_species_rasts[run],
                                                       inf_species_rasts[run],
                                                       inf_species_cohort_rasts[run][current_age],
                                                       lvtree_rast,
                                                       outside_spores[run],
                                                       weather || moisture_temperature,
                                                       weather_coefficients[step],
                                                       kernels[run]);
                        }
                    }
                    unresolved_steps.clear();
                    unresolved_dates.clear();
                }
                for (unsigned run = 0; run < num_runs; run++) {
                    last_checkpoint = dd_current.year() - dd_start.year() + 1;
                    sus_checkpoint[last_checkpoint][run] = sus_species_rasts[run];
                    inf_checkpoint[last_checkpoint][run] = inf_species_rasts[run];
                    step_checkpoint[last_checkpoint] = current_step;
                    date_checkpoint[last_checkpoint] = dd_current;
                }
                if (mortality) {
                    // TODO: use the library code to handle mortality
                    unsigned simulation_year = dd_current.year() - dd_start.year();
                    // only run to the current year of simulation
                    // (first year is 0):
                    //   max index == sim year
                    // reduced by first time when trees start dying
                    // (counted from 1: first year == 1)
                    // e.g. for sim year 3, year dying 4, max index is 0
                    //   max index = sim year - (dying year - 1)
                    // index is negative before we reach the year
                    // (so we can skip these years)
                    // sim year - (dying year - 1) < 0
                    // sim year < dying year - 1
                    if (simulation_year >= first_year_to_die - 1) {
                        auto max_index = simulation_year - (first_year_to_die - 1);
                        #pragma omp parallel for num_threads(threads)
                        for (unsigned run = 0; run < num_runs; run++) {
                            dead_in_current_year[run].zero();
                            for (unsigned age = 0; age <= max_index; age++) {
                                Img dead_in_cohort = infected_to_dead_rate * inf_species_cohort_rasts[run][age];
                                inf_species_cohort_rasts[run][age] -= dead_in_cohort;
                                dead_in_current_year[run] += dead_in_cohort;
                            }
                            inf_species_rasts[run] -= dead_in_current_year[run];
                        }
                    }
                }
                // compute spread rate
                if (opt.spread_rate_output->answer) {
                    unsigned simulation_year = dd_current.year() - dd_start.year();
                    #pragma omp parallel for num_threads(threads)
                    for (unsigned i = 0; i < num_runs; i++) {
                        spread_rates[i].compute_yearly_spread_rate(inf_species_rasts[i], simulation_year);
                    }
                }
                if (sync) {
                    unsigned selected_run = 0;
                    // sync infectious and susceptible in all threads to selected one
                    for (unsigned run = 0; run < num_runs; run++) {
                        if (run != selected_run) {
                            sus_species_rasts[run] = sus_species_rasts[selected_run];
                            inf_species_rasts[run] = inf_species_rasts[selected_run];
                        }
                    }
                    sync = false;
                    if (opt.spread_rate_output->answer) {
                        unsigned num_years_spread = dd_current.year() - dd_start.year() + 1;
                        write_spread_rate(opt.spread_rate_output, spread_rates[selected_run],
                                          num_years_spread, start_time);
                    }
                }
                else {
                    if (opt.spread_rate_output->answer) {
                        unsigned num_years_spread = dd_current.year() - dd_start.year() + 1;
                        write_spread_rate(opt.spread_rate_output, spread_rates,
                                          num_years_spread, start_time);
                    }
                }
                if ((opt.output_series->answer && !flg.series_as_single_run->answer)
                        || opt.stddev_series->answer) {
                    // aggregate in the series
                    I_species_rast.zero();
                    for (unsigned i = 0; i < num_runs; i++)
                        I_species_rast += inf_species_rasts[i];
                    I_species_rast /= num_runs;
                }
                if (opt.output_series->answer) {
                    // write result
                    // date is always end of the year, even for seasonal spread
                    string name = generate_name(opt.output_series->answer, dd_current_last_day);
                    if (flg.series_as_single_run->answer)
                        raster_to_grass(inf_species_rasts[0], name,
                            "Occurrence from a single stochastic run",
                            dd_current_last_day);
                    else
                        raster_to_grass(I_species_rast, name,
                                        "Average occurrence from a all stochastic runs",
                                        dd_current_last_day);
                    if (steering)
                        c.send_data("output:" + name + '|');
                    last_name = name;
                    G_verbose_message("Output raster %s written", name.c_str());
                }
                if (opt.stddev_series->answer) {
                    Img stddev(I_species_rast, 0);
                    for (unsigned i = 0; i < num_runs; i++) {
                        Img tmp = inf_species_rasts[i] - I_species_rast;
                        stddev += tmp * tmp;
                    }
                    stddev /= num_runs;
                    stddev.for_each([](int& a){a = std::sqrt(a);});
                    string name = generate_name(opt.stddev_series->answer, dd_current_last_day);
                    string title = "Standard deviation of average"
                                   " occurrence from a all stochastic runs";
                    raster_to_grass(stddev, name, title, dd_current_last_day);
                    G_verbose_message("Output raster %s written", name.c_str());
                }
                if (opt.probability_series->answer) {
                    Img probability(I_species_rast, 0);
                    for (unsigned i = 0; i < num_runs; i++) {
                        Img tmp = inf_species_rasts[i];
                        tmp.for_each([](int& a){a = bool(a);});
                        probability += tmp;
                    }
                    probability *= 100;  // prob from 0 to 100 (using ints)
                    probability /= num_runs;
                    string name = generate_name(opt.probability_series->answer, dd_current_last_day);
                    string title = "Probability of occurrence";
                    raster_to_grass(probability, name, title, dd_current_last_day);
                    if (steering)
                        c.send_data("output:" + name + '|');
                    G_verbose_message("Output raster %s written", name.c_str());
                }
                if (mortality && opt.dead_series->answer) {
                    accumulated_dead += dead_in_current_year[0];
                    if (opt.dead_series->answer) {
                        string name = generate_name(opt.dead_series->answer, dd_current_last_day);
                        raster_to_grass(accumulated_dead, name,
                                        "Number of dead hosts to date",
                                        dd_current_last_day);
                        G_verbose_message("Output raster %s written", name.c_str());
                    }
                }
            }
            if (after_loading_checkpoint)
                after_loading_checkpoint = false;
            if (step_type == "month")
                dd_current.increased_by_month();
            else
                dd_current.increased_by_week();
            current_step += 1;
            if (dd_current > dd_end) {
                if (steering)
                    c.send_data("info:last:" + last_name);
                else
                    break;
            }
        }
        else {
            // paused
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (opt.output->answer || opt.stddev->answer) {
        // aggregate
        I_species_rast.zero();
        for (unsigned i = 0; i < num_runs; i++)
            I_species_rast += inf_species_rasts[i];
        I_species_rast /= num_runs;
    }
    if (opt.output->answer) {
        // write final result
        raster_to_grass(I_species_rast, opt.output->answer,
                        "Average occurrence from a all stochastic runs",
                        dd_current_last_day);
        G_verbose_message("Final output raster %s written", opt.output->answer);
    }
    if (opt.stddev->answer) {
        Img stddev(I_species_rast, 0);
        for (unsigned i = 0; i < num_runs; i++) {
            Img tmp = inf_species_rasts[i] - I_species_rast;
            stddev += tmp * tmp;
        }
        stddev /= num_runs;
        stddev.for_each([](int& a){a = std::sqrt(a);});
        raster_to_grass(stddev, opt.stddev->answer,
                        opt.stddev->description, dd_current_last_day);
        G_verbose_message("Final output raster %s written", opt.stddev->answer);
    }
    if (opt.probability->answer) {
        Img probability(I_species_rast, 0);
        for (unsigned i = 0; i < num_runs; i++) {
            Img tmp = inf_species_rasts[i];
            tmp.for_each([](int& a){a = bool(a);});
            probability += tmp;
        }
        probability *= 100;  // prob from 0 to 100 (using ints)
        probability /= num_runs;
        raster_to_grass(probability, opt.probability->answer,
                        "Probability of occurrence", dd_current_last_day);
        G_verbose_message("Final output raster %s written", opt.probability->answer);
    }
    if (opt.outside_spores->answer) {
        Cell_head region;
        Rast_get_window(&region);
        struct Map_info Map;
        struct line_pnts *Points;
        struct line_cats *Cats;
        if (Vect_open_new(&Map, opt.outside_spores->answer, WITHOUT_Z) < 0)
            G_fatal_error(_("Unable to create vector map <%s>"), opt.outside_spores->answer);

        Points = Vect_new_line_struct();
        Cats = Vect_new_cats_struct();

        for (unsigned i = 0; i < num_runs; i++) {
            for (unsigned j = 0; j < outside_spores[i].size(); j++) {
                int row = std::get<0>(outside_spores[i][j]);
                int col = std::get<1>(outside_spores[i][j]);
                double n = Rast_row_to_northing(row, &region);
                double e = Rast_col_to_easting(col, &region);
                Vect_reset_line(Points);
                Vect_reset_cats(Cats);
                Vect_append_point(Points, e, n, 0);
                Vect_cat_set(Cats, 1, i + 1);
                Vect_write_line(&Map, GV_POINT, Points, Cats);
            }
        }
        Vect_hist_command(&Map);
        Vect_set_map_name(
                    &Map,
                    "Dispersers escaped outside computational region");
        Vect_write_header(&Map);
        Vect_build(&Map);
        Vect_close(&Map);
        struct TimeStamp timestamp;
        date_to_grass(dd_current_last_day, &timestamp);
        G_write_vector_timestamp(opt.outside_spores->answer,
                                 NULL, &timestamp);
        Vect_destroy_line_struct(Points);
        Vect_destroy_cats_struct(Cats);
    }

    if (steering) {
        client_thread.join();
        c.close_socket();
    }
    G_verbose_message("Simulation ended");
    return 0;
}
