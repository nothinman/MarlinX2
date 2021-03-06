/*
 planner.c - buffers movement commands and manages the acceleration profile plan
 Part of Grbl
 
 Copyright (c) 2009-2011 Simen Svale Skogsrud
 
 Grbl is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 Grbl is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
 */

/* The ring buffer implementation gleaned from the wiring_serial library by David A. Mellis. */

/*  
 Reasoning behind the mathematics in this module (in the key of 'Mathematica'):
 
 s == speed, a == acceleration, t == time, d == distance
 
 Basic definitions:
 
 Speed[s_, a_, t_] := s + (a*t) 
 Travel[s_, a_, t_] := Integrate[Speed[s, a, t], t]
 
 Distance to reach a specific speed with a constant acceleration:
 
 Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, d, t]
 d -> (m^2 - s^2)/(2 a) --> estimate_acceleration_distance()
 
 Speed after a given distance of travel with constant acceleration:
 
 Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, m, t]
 m -> Sqrt[2 a d + s^2]    
 
 DestinationSpeed[s_, a_, d_] := Sqrt[2 a d + s^2]
 
 When to start braking (di) to reach a specified destionation speed (s2) after accelerating
 from initial speed s1 without ever stopping at a plateau:
 
 Solve[{DestinationSpeed[s1, a, di] == DestinationSpeed[s2, a, d - di]}, di]
 di -> (2 a d - s1^2 + s2^2)/(4 a) --> intersection_distance()
 
 IntersectionDistance[s1_, s2_, a_, d_] := (2 a d - s1^2 + s2^2)/(4 a)
 */

#include "Marlin.h"
#include "planner.h"
#include "stepper.h"
#include "temperature.h"
#include "ultralcd.h"
#include "language.h"

//===========================================================================
//=============================public variables ============================
//===========================================================================

unsigned long minsegmenttime;
float max_feedrate[3 + EXTRUDERS]; // set the max speeds
float axis_steps_per_unit[3 + EXTRUDERS];
unsigned long max_acceleration_units_per_sq_second[3 + EXTRUDERS]; // Use M201 to override by software
float minimumfeedrate;
float acceleration;         // Normal acceleration mm/s^2  THIS IS THE DEFAULT ACCELERATION for all moves. M204 SXXXX
float retract_acceleration[EXTRUDERS]; // mm/s^2, per extruder filament pull-pack and push-forward  while standing still in the other axis M204 TXXXX
float max_e_jerk[EXTRUDERS]; // mm/s - initial speed for extruder retract moves
float max_xy_jerk; // speed than can be stopped at once, if i understand correctly.
float max_z_jerk;
float mintravelfeedrate;
uint8_t last_extruder;

// The current position of the tool in absolute steps
long position[NUM_AXIS];   //rescaled from extern when axis_steps_per_unit are changed by gcode or extruder changes
static float previous_speed[NUM_AXIS]; // Speed of previous path line segment
static float previous_nominal_speed; // Nominal speed of previous path line segment
static unsigned long axis_steps_per_sqr_second[NUM_AXIS]; // acceleration in steps

#ifdef AUTOTEMP
float autotemp_max=250;
float autotemp_min=210;
float autotemp_factor=0.1;
bool autotemp_enabled=false;
#endif

//===========================================================================
//=================semi-private variables, used in inline  functions    =====
//===========================================================================
block_t block_buffer[BLOCK_BUFFER_SIZE];            // A ring buffer for motion instfructions
volatile unsigned char block_buffer_head;           // Index of the next block to be pushed
volatile unsigned char block_buffer_tail;           // Index of the block to process now

//===========================================================================
//=============================private variables ============================
//===========================================================================
#ifdef PREVENT_DANGEROUS_EXTRUDE
bool allow_cold_extrude=false;
#endif
#ifdef XY_FREQUENCY_LIMIT
#define MAX_FREQ_TIME (1000000.0/XY_FREQUENCY_LIMIT)
// Used for the frequency limit
static unsigned char old_direction_bits = 0;               // Old direction bits. Used for speed calculations
static long x_segment_time[3]={MAX_FREQ_TIME + 1,0,0};     // Segment times (in us). Used for speed calculations
static long y_segment_time[3]={MAX_FREQ_TIME + 1,0,0};
#endif

// Returns the index of the next block in the ring buffer
// NOTE: Removed modulo (%) operator, which uses an expensive divide and multiplication.
static int8_t next_block_index(int8_t block_index) {
  block_index++;
  if (block_index == BLOCK_BUFFER_SIZE) { 
    block_index = 0; 
  }
  return(block_index);
}


// Returns the index of the previous block in the ring buffer
static int8_t prev_block_index(int8_t block_index) {
  if (block_index == 0) { 
    block_index = BLOCK_BUFFER_SIZE; 
  }
  block_index--;
  return(block_index);
}

//===========================================================================
//=============================functions         ============================
//===========================================================================

// Calculates the distance (not time) it takes to accelerate from initial_rate to target_rate using the 
// given acceleration:
FORCE_INLINE float estimate_acceleration_distance(float initial_rate, float target_rate, float acceleration)
{
  if (acceleration!=0) {
    return((target_rate*target_rate-initial_rate*initial_rate)/
      (2.0*acceleration));
  }
  else {
    return 0.0;  // acceleration was 0, set acceleration distance to 0
  }
}

// This function gives you the point at which you must start braking (at the rate of -acceleration) if 
// you started at speed initial_rate and accelerated until this point and want to end at the final_rate after
// a total travel of distance. This can be used to compute the intersection point between acceleration and
// deceleration in the cases where the trapezoid has no plateau (i.e. never reaches maximum speed)

FORCE_INLINE float intersection_distance(float initial_rate, float final_rate, float acceleration, float distance) 
{
  if (acceleration!=0) {
    return((2.0*acceleration*distance-initial_rate*initial_rate+final_rate*final_rate)/
      (4.0*acceleration) );
  }
  else {
    return 0.0;  // acceleration was 0, set intersection distance to 0
  }
}

// Calculates the maximum allowable speed at this point when you must be able to reach target_velocity using the 
// acceleration within the allotted distance.
FORCE_INLINE float max_allowable_speed(float acceleration, float target_velocity, float distance) {
  return  sqrt(target_velocity*target_velocity-2*acceleration*distance);
}

#ifdef C_COMPENSATION
// Calculate compensation (in steps) for given E speeds and extruder
FORCE_INLINE void calc_c_comp(unsigned long s1, long &c1, 
                              unsigned long s2, long &c2, 
                              unsigned long s3, long &c3, 
                              uint8_t extruder)
{
  float low_bound = 0;
  float low_comp = 0;
  c1 = c2 = c3 = 0;
  for(int ii = 0; ii < gCComp_size[extruder]; ii++) 
  {
    if(s1 < low_bound && s2 < low_bound && s3 < low_bound) {
      break; 
    }
    float high_bound = gCComp[ii][extruder][0] * axis_steps_per_unit[E_AXIS + extruder];
    float high_comp = gCComp[ii][extruder][1] * axis_steps_per_unit[E_AXIS + extruder];
    float a = (low_comp - high_comp)/(low_bound - high_bound);
    float b = (high_bound*low_comp - low_bound*high_comp)/(high_bound - low_bound);
    if(s2 >= low_bound && s2 < high_bound) {
      c2 = floor(a*s2 + b);
    } else if(s2 > high_bound) {
      c2 = floor(high_comp);
    }
    #ifdef C_COMPENSATION_IGNORE_ACCELERATION
    c1 = c3 = c2;
    #else // C_COMPENSATION_IGNORE_ACCELERATION
    if(s1 >= low_bound && s1 < high_bound) {
      c1 = floor(a*s1 + b);
    } else if(s1 > high_bound) {
      c1 = floor(high_comp);
    }
    if(s3 >= low_bound && s3 < high_bound) {
      c3 = floor(a*s3 + b);
    } else if(s3 > high_bound) {
      c3 = floor(high_comp);
    }
    #endif // C_COMPENSATION_IGNORE_ACCELERATION
    low_bound = high_bound;
    low_comp = high_comp;
  }
  return;
}
#endif // C_COMPENSATION

// Calculates trapezoid parameters so that the entry- and exit-speed is compensated by the provided factors.
void calculate_trapezoid_for_block(block_t *block, float entry_factor, float exit_factor) {
  unsigned long initial_rate = ceil(block->nominal_rate*entry_factor); // (step/min)
  unsigned long final_rate = ceil(block->nominal_rate*exit_factor); // (step/min)
  unsigned long target_rate = block->nominal_rate; // (step/min)

  // Limit minimal step rate (Otherwise the timer will overflow.)
  if(initial_rate <120) {
    initial_rate=120; 
  }
  if(final_rate < 120) {
    final_rate=120;  
  }
  
  // Make sure the final rate is not higer that nominal
  if(final_rate > target_rate) {
    final_rate = target_rate;
  }

  long acceleration = block->acceleration_st;
  int32_t accelerate_steps =
    ceil(estimate_acceleration_distance(block->initial_rate, target_rate, acceleration));
  int32_t decelerate_steps =
    floor(estimate_acceleration_distance(target_rate, block->final_rate, -acceleration));

  // Calculate the size of Plateau of Nominal Rate.
  int32_t plateau_steps = block->step_event_count-accelerate_steps-decelerate_steps;

  // Is the Plateau of Nominal Rate smaller than nothing? That means no cruising, and we will
  // have to use intersection_distance() to calculate when to abort acceleration and start braking
  // in order to reach the final_rate exactly at the end of this block.
  if (plateau_steps < 0) {
    accelerate_steps = ceil(intersection_distance(block->initial_rate, block->final_rate, acceleration, block->step_event_count));
    accelerate_steps = max(accelerate_steps,0); // Check limits due to numerical round-off
    accelerate_steps = min((uint32_t)accelerate_steps,block->step_event_count);//(We can cast here to unsigned, because the above line ensures that we are above zero)
    target_rate = max_allowable_speed(block->initial_rate, -acceleration, accelerate_steps);
    plateau_steps = 0;
  }

#ifdef C_COMPENSATION
  long initial_advance;
  long target_advance;
  long final_advance;
  float e_factor = (float)block->steps_e / (float)block->step_event_count;
  // Set filament compensation values in steps (only if moving in X, Y and positive E)
  if((block->steps_x > dropsegments || block->steps_y > dropsegments) && 
     (block->steps_e > 0 && (block->direction_bits & (1<<E_AXIS)) == 0))
  {
    calc_c_comp(initial_rate * e_factor, initial_advance, 
                target_rate * e_factor, target_advance, 
                final_rate * e_factor, final_advance,
                block->active_extruder);
  } 
  else // For all other blocks keep compensation unchanged
  {
    initial_advance = target_advance = final_advance = block->prev_advance;
  }
#endif // C_COMPENSATION

  CRITICAL_SECTION_START;  // Fill variables used by the stepper in a critical section
  if(block->busy == false) { // Don't update variables if block is busy.
    block->accelerate_until = accelerate_steps;
    block->decelerate_after = accelerate_steps+plateau_steps;
    block->initial_rate = initial_rate;
    block->final_rate = final_rate;
#ifdef C_COMPENSATION
    block->initial_advance = initial_advance;
    block->final_advance = final_advance;
    block->target_advance = target_advance;
#endif // C_COMPENSATION
  }
  CRITICAL_SECTION_END;
}                    


// The kernel called by planner_recalculate() when scanning the plan from last to first entry.
void planner_reverse_pass_kernel(block_t *previous, block_t *current, block_t *next) 
{
  if(!current) { 
    return; 
  }

  if (next) {
    // If entry speed is already at the maximum entry speed, no need to recheck. Block is cruising.
    // If not, block in state of acceleration or deceleration. Reset entry speed to maximum and
    // check for maximum allowable speed reductions to ensure maximum possible planned speed.
    if (current->entry_speed != current->max_entry_speed) {

      // If nominal length true, max junction speed is guaranteed to be reached. Only compute
      // for max allowable speed if block is decelerating and nominal length is false.
      if ((!current->nominal_length_flag) && (current->max_entry_speed > next->entry_speed)) {
        current->entry_speed = min( current->max_entry_speed,
        max_allowable_speed(-current->acceleration,next->entry_speed,current->millimeters));
      } 
      else {
        current->entry_speed = current->max_entry_speed;
      }
      current->recalculate_flag = true;

    }
  } // Skip last block. Already initialized and set for recalculation.
}

// planner_recalculate() needs to go over the current plan twice. Once in reverse and once forward. This 
// implements the reverse pass.
void planner_reverse_pass() {
  uint8_t block_index = block_buffer_head;
  
  //Make a local copy of block_buffer_tail, because the interrupt can alter it
  CRITICAL_SECTION_START;
  unsigned char tail = block_buffer_tail;
  CRITICAL_SECTION_END
  
  if(((block_buffer_head-tail + BLOCK_BUFFER_SIZE) & (BLOCK_BUFFER_SIZE - 1)) > 3) {
    block_index = (block_buffer_head - 3) & (BLOCK_BUFFER_SIZE - 1);
    block_t *block[3] = { 
      NULL, NULL, NULL         };
    while(block_index != tail) { 
      block_index = prev_block_index(block_index); 
      block[2]= block[1];
      block[1]= block[0];
      block[0] = &block_buffer[block_index];
      planner_reverse_pass_kernel(block[0], block[1], block[2]);
    }
  }
}

// The kernel called by planner_recalculate() when scanning the plan from first to last entry.
void planner_forward_pass_kernel(block_t *previous, block_t *current, block_t *next) {
  if(!previous) { 
    return; 
  }

  // If the previous block is an acceleration block, but it is not long enough to complete the
  // full speed change within the block, we need to adjust the entry speed accordingly. Entry
  // speeds have already been reset, maximized, and reverse planned by reverse planner.
  // If nominal length is true, max junction speed is guaranteed to be reached. No need to recheck.
  if (!previous->nominal_length_flag) {
    if (previous->entry_speed < current->entry_speed) {
      double entry_speed = min( current->entry_speed,
      max_allowable_speed(-previous->acceleration,previous->entry_speed,previous->millimeters) );

      // Check for junction speed change
      if (current->entry_speed != entry_speed) {
        current->entry_speed = entry_speed;
        current->recalculate_flag = true;
      }
    }
  }
}

// planner_recalculate() needs to go over the current plan twice. Once in reverse and once forward. This 
// implements the forward pass.
void planner_forward_pass() {
  uint8_t block_index = block_buffer_tail;
  block_t *block[3] = { 
    NULL, NULL, NULL   };

  while(block_index != block_buffer_head) {
    block[0] = block[1];
    block[1] = block[2];
    block[2] = &block_buffer[block_index];
    planner_forward_pass_kernel(block[0],block[1],block[2]);
    block_index = next_block_index(block_index);
  }
  planner_forward_pass_kernel(block[1], block[2], NULL);
}

#ifdef ENABLE_DEBUG
void planner_print_plan() {
  int8_t block_index = block_buffer_tail;
  block_t *current = NULL;
  while(block_index != block_buffer_head) {
    current = &block_buffer[block_index];
    SERIAL_ECHO_START;
    SERIAL_ECHOPAIR("I:", (int)block_index);
    SERIAL_ECHOPAIR(" AE:", (int)current->active_extruder);
    SERIAL_ECHOPAIR(" ES:", current->entry_speed);
    SERIAL_ECHOPAIR(" NS:", current->nominal_speed);
    SERIAL_ECHOPAIR(" TD:", current->millimeters);
    SERIAL_ECHOPAIR(" AC:", current->acceleration);
    SERIAL_ECHOPAIR(" SC:", current->step_event_count);
    SERIAL_ECHOPAIR(" SX:", current->steps_x);
    SERIAL_ECHOPAIR(" SY:", current->steps_y);
    SERIAL_ECHOPAIR(" SE:", current->steps_e);
    #ifdef C_COMPENSATION
    SERIAL_ECHOPAIR(" IA:", current->initial_advance);
    SERIAL_ECHOPAIR(" TA:", current->target_advance);
    SERIAL_ECHOPAIR(" FA:", current->final_advance);
    SERIAL_ECHOPAIR(" PA:", current->prev_advance);
    SERIAL_ECHOPAIR(" NA:", current->next_advance);
    #endif // C_COMPENSATION
    SERIAL_ECHOLN("");
    block_index = next_block_index( block_index );
  }
}
#endif // ENABLE_DEBUG

// Recalculates the trapezoid speed profiles for all blocks in the plan according to the 
// entry_factor for each junction. Must be called by planner_recalculate() after 
// updating the blocks.
void planner_recalculate_trapezoids() {
  int8_t block_index = block_buffer_tail;
  block_t *prev = NULL;
  block_t *current = NULL;
  block_t *next = NULL;

  while(block_index != block_buffer_head) {
    prev = current;
    current = next;
    next = &block_buffer[block_index];
    if (current) {
      // Recalculate if current block entry or exit junction speed has changed.
      if (current->recalculate_flag || next->recalculate_flag) {
        #ifdef C_COMPENSATION
        if(prev) {
          current->prev_advance = prev->final_advance;
        } 
        #endif // C_COMPENSATION
        // NOTE: Entry and exit factors always > 0 by all previous logic operations.
        calculate_trapezoid_for_block(current, current->entry_speed/current->nominal_speed,
                                      next->entry_speed/current->nominal_speed);
        current->recalculate_flag = false; // Reset current only to ensure next trapezoid is computed
        #ifdef C_COMPENSATION
        if(prev && prev->next_advance != current->initial_advance) {
          prev->next_advance = current->initial_advance;
        }
        #endif // C_COMPENSATION
      }
    }
    block_index = next_block_index( block_index );
  }
  // Last/newest block in buffer. Exit speed is set with MINIMUM_PLANNER_SPEED. Always recalculated.
  if(next != NULL) {
    #ifdef C_COMPENSATION
    if(current) {
      next->prev_advance = current->final_advance;
    }
    #endif // C_COMPENSATION
    calculate_trapezoid_for_block(next, next->entry_speed/next->nominal_speed,
                                  MINIMUM_PLANNER_SPEED/next->nominal_speed);
    next->recalculate_flag = false;
    #ifdef C_COMPENSATION
    if(current) {
      current->next_advance = next->initial_advance;
    }
    #endif // C_COMPENSATION
  }
}

// Recalculates the motion plan according to the following algorithm:
//
//   1. Go over every block in reverse order and calculate a junction speed reduction (i.e. block_t.entry_factor) 
//      so that:
//     a. The junction jerk is within the set limit
//     b. No speed reduction within one block requires faster deceleration than the one, true constant 
//        acceleration.
//   2. Go over every block in chronological order and dial down junction speed reduction values if 
//     a. The speed increase within one block would require faster accelleration than the one, true 
//        constant acceleration.
//
// When these stages are complete all blocks have an entry_factor that will allow all speed changes to 
// be performed using only the one, true constant acceleration, and where no junction jerk is jerkier than 
// the set limit. Finally it will:
//
//   3. Recalculate trapezoids for all blocks.

void planner_recalculate() {   
  planner_reverse_pass();
  planner_forward_pass();
  planner_recalculate_trapezoids();
}

void plan_init() {
  block_buffer_head = 0;
  block_buffer_tail = 0;
  memset(position, 0, sizeof(position)); // clear position
  previous_speed[0] = 0.0;
  previous_speed[1] = 0.0;
  previous_speed[2] = 0.0;
  previous_speed[3] = 0.0; // should stay unused
  previous_nominal_speed = 0.0;
}

#ifdef AUTOTEMP
void getHighESpeed()
{
  static float oldt=0;
  if(!autotemp_enabled){
    return;
  }
  if(degTargetHotend0()+2<autotemp_min) {  //probably temperature set to zero.
    return; //do nothing
  }

  float high=0.0;
  uint8_t block_index = block_buffer_tail;

  while(block_index != block_buffer_head) {
    if((block_buffer[block_index].steps_x != 0) ||
      (block_buffer[block_index].steps_y != 0) ||
      (block_buffer[block_index].steps_z != 0)) {
      float se=(float(block_buffer[block_index].steps_e)/float(block_buffer[block_index].step_event_count))*block_buffer[block_index].nominal_speed;
      //se; mm/sec;
      if(se>high)
      {
        high=se;
      }
    }
    block_index = (block_index+1) & (BLOCK_BUFFER_SIZE - 1);
  }

  float g=autotemp_min+high*autotemp_factor;
  float t=g;
  if(t<autotemp_min)
    t=autotemp_min;
  if(t>autotemp_max)
    t=autotemp_max;
  if(oldt>t)
  {
    t=AUTOTEMP_OLDWEIGHT*oldt+(1-AUTOTEMP_OLDWEIGHT)*t;
  }
  oldt=t;
  setTargetHotend0(t);
}
#endif

void check_axes_activity()
{
  unsigned char x_active = 0;
  unsigned char y_active = 0;  
  unsigned char z_active = 0;
  unsigned char e_active = 0;
  unsigned char tail_fan_speed[EXTRUDERS];
  block_t *block;

  memcpy(tail_fan_speed, fanSpeed, sizeof(tail_fan_speed));
  if(block_buffer_tail != block_buffer_head)
  {
    uint8_t block_index = block_buffer_tail;
    block = &block_buffer[block_index];
    tail_fan_speed[block->active_extruder] = block->fan_speed;
    while(block_index != block_buffer_head)
    {
      block = &block_buffer[block_index];
      if(block->steps_x != 0) x_active++;
      if(block->steps_y != 0) y_active++;
      if(block->steps_z != 0) z_active++;
      if(block->steps_e != 0) e_active++;
      block_index = (block_index+1) & (BLOCK_BUFFER_SIZE - 1);
    }
  }
  if((DISABLE_X) && (x_active == 0)) disable_x();
  if((DISABLE_Y) && (y_active == 0)) disable_y();
  if((DISABLE_Z) && (z_active == 0)) disable_z();
  if((DISABLE_E) && (e_active == 0))
  {
    disable_e0();
    disable_e1();
    disable_e2(); 
  }
#ifndef FAN_SOFT_PWM
  for(uint8_t e = 0; e < EXTRUDERS; e++) 
  {
    #if defined(PER_EXTRUDER_FANS) && EXTRUDERS > 1
    // Use fan speed of the active extruder for the followers
    if(follow_me_fan && (follow_me & (1<<e)) != 0) {
       tail_fan_speed[e] = tail_fan_speed[ACTIVE_EXTRUDER];
       fanSpeed[e] = fanSpeed[ACTIVE_EXTRUDER];
    }
    #endif // PER_EXTRUDER_FANS && EXTRUDERS > 1
    #ifdef FAN_KICKSTART_TIME
    static unsigned long fan_kick_end[EXTRUDERS];
    static uint8_t fan_prev_speed[EXTRUDERS];
    #ifndef PER_EXTRUDER_FANS
    if(e == ACTIVE_EXTRUDER) 
    #endif // PER_EXTRUDER_FANS
      if (tail_fan_speed[e] > fan_prev_speed[e]) {
        if (fan_kick_end[e] == 0) {
          // Starting up or bumping up the speed, kick start it
          fan_kick_end[e] = millis() + FAN_KICKSTART_TIME;
          tail_fan_speed[e] = 255;
        } else if (fan_kick_end[e] > millis()) {
          // Fan still spinning up.
          tail_fan_speed[e] = 255;
        } else {
          // Done spinning up
          fan_prev_speed[e] = tail_fan_speed[e];
        }
      } else {
        fan_kick_end[e] = 0;
        fan_prev_speed[e] = tail_fan_speed[e];
      }
    #endif//FAN_KICKSTART_TIME
    #ifdef PER_EXTRUDER_FANS
    if(fan_pin[e] > -1) {
      analogWrite(fan_pin[e], tail_fan_speed[e]);
      #ifdef ENABLE_DEBUG
      if((debug_flags & FAN_DEBUG) != 0 && (millis() & 0x1f) == 0) {
        SERIAL_ECHO_START;
        SERIAL_ECHO(" FAN_DEBUG Ext");
        SERIAL_ECHO(e);
        SERIAL_ECHO(": PWM:");
        SERIAL_ECHOLN((int)tail_fan_speed[e]);
      }
      #endif //ENABLE_DEBUG
    }
    #else // PER_EXTRUDER_FANS
    if(e == ACTIVE_EXTRUDER && FAN_PIN > -1) {
      analogWrite(FAN_PIN, tail_fan_speed[e]);
      #ifdef ENABLE_DEBUG
      if((debug_flags & FAN_DEBUG) != 0 && (millis() & 0x1f) == 0) {
        SERIAL_ECHO_START;
        SERIAL_ECHO(" FAN_DEBUG PWM:");
        SERIAL_ECHOLN((int)tail_fan_speed[e]);
      }
      #endif //ENABLE_DEBUG
    }
    #endif // PER_EXTRUDER_FANS
  } // end extruder loop
#endif // ! FAN_SOFT_PWM
#ifdef AUTOTEMP
  getHighESpeed();
#endif
}


float junction_deviation = 0.1;
// Add a new linear movement to the buffer. steps_x, _y and _z is the absolute position in 
// mm. Microseconds specify how many microseconds the move should take to perform. To aid acceleration
// calculation the caller must also provide the physical length of the line in millimeters.
void plan_buffer_line(const float &x, const float &y, const float &z, const float &e, float feed_rate, const uint8_t &extruder)
{
  // Calculate the buffer head after we push this byte
  int next_buffer_head = next_block_index(block_buffer_head);

  // If the buffer is full: good! That means we are well ahead of the robot. 
  // Rest here until there is room in the buffer.
  while(block_buffer_tail == next_buffer_head)
  {
    manage_heater(); 
    manage_inactivity(); 
    lcd_update();
  }
  
  // The target position of the tool in absolute steps
  // Calculate target position in absolute steps
  //this should be done after the wait, because otherwise a M92 code within the gcode disrupts this calculation somehow
  long target[4];
  target[X_AXIS] = lround(x*axis_steps_per_unit[X_AXIS]);
  target[Y_AXIS] = lround(y*axis_steps_per_unit[Y_AXIS]);
  target[Z_AXIS] = lround(z*axis_steps_per_unit[Z_AXIS]);     
  target[E_AXIS] = lround(e*axis_steps_per_unit[E_AXIS + extruder]);

  // If changing extruder have to recalculate current position based on 
  // the steps-per-mm value for the new extruder.
  #if EXTRUDERS > 1
  if(last_extruder != extruder && axis_steps_per_unit[E_AXIS + extruder] != 
                                  axis_steps_per_unit[E_AXIS + last_extruder]) {
    float factor = float(axis_steps_per_unit[E_AXIS + extruder]) /
                   float(axis_steps_per_unit[E_AXIS + last_extruder]);
    position[E_AXIS] = lround(position[E_AXIS] * factor);
  }
  #endif

  #ifdef PREVENT_DANGEROUS_EXTRUDE
  if(target[E_AXIS]!=position[E_AXIS])
  {
    #ifdef EXTRUDE_MINTEMP
    if(degHotend(active_extruder)<EXTRUDE_MINTEMP && !allow_cold_extrude)
    {
      position[E_AXIS]=target[E_AXIS]; //behave as if the move really took place, but ignore E part
      SERIAL_ECHO_START;
      SERIAL_ECHOLNPGM(MSG_ERR_COLD_EXTRUDE_STOP);
    }
    #endif
    #ifdef EXTRUDE_MAXLENGTH
    if(labs(target[E_AXIS]-position[E_AXIS])>axis_steps_per_unit[E_AXIS]*EXTRUDE_MAXLENGTH)
    {
      position[E_AXIS]=target[E_AXIS]; //behave as if the move really took place, but ignore E part
      SERIAL_ECHO_START;
      SERIAL_ECHOLNPGM(MSG_ERR_LONG_EXTRUDE_STOP);
    }
    #endif
  }
  #endif

  // Prepare to set up new block
  block_t *block = &block_buffer[block_buffer_head];

  // Mark block as not busy (Not executed by the stepper interrupt)
  block->busy = false;

  // Number of steps for each axis
  block->steps_x = labs(target[X_AXIS]-position[X_AXIS]);
  block->steps_y = labs(target[Y_AXIS]-position[Y_AXIS]);
  block->steps_z = labs(target[Z_AXIS]-position[Z_AXIS]);
  block->steps_e = labs(target[E_AXIS]-position[E_AXIS]);
  block->steps_e *= extrudemultiply;
  block->steps_e /= 100;
  block->step_event_count = max(block->steps_x, max(block->steps_y, max(block->steps_z, block->steps_e)));

  // Bail if this is a zero-length block
  if (block->step_event_count <= dropsegments)
  { 
    return; 
  }

  block->fan_speed = fanSpeed[extruder];

  // Compute direction bits for this block 
  block->direction_bits = 0;
  if (target[X_AXIS] < position[X_AXIS])
  {
    block->direction_bits |= (1<<X_AXIS); 
  }
  if (target[Y_AXIS] < position[Y_AXIS])
  {
    block->direction_bits |= (1<<Y_AXIS); 
  }
  if (target[Z_AXIS] < position[Z_AXIS])
  {
    block->direction_bits |= (1<<Z_AXIS); 
  }
  if (target[E_AXIS] < position[E_AXIS])
  {
    block->direction_bits |= (1<<E_AXIS); 
  }

  block->active_extruder = extruder;

  //enable active axes
  if(block->steps_x != 0) {
    enable_x();
  }
  if(block->steps_y != 0) {
    enable_y();
  }
#ifndef Z_LATE_ENABLE
  if(block->steps_z != 0) enable_z();
#endif

  // Enable all
  if(block->steps_e != 0)
  {
    enable_e0();
    enable_e1();
    enable_e2(); 
  }

  if (block->steps_e == 0)
  {
    if(feed_rate<mintravelfeedrate) feed_rate=mintravelfeedrate;
    block->travel = true;
  }
  else
  {
    if(feed_rate<minimumfeedrate) feed_rate=minimumfeedrate;
    block->travel = false;
  } 

  bool no_move;
  float delta_mm[4];
  delta_mm[X_AXIS] = (target[X_AXIS]-position[X_AXIS])/axis_steps_per_unit[X_AXIS];
  delta_mm[Y_AXIS] = (target[Y_AXIS]-position[Y_AXIS])/axis_steps_per_unit[Y_AXIS];
  delta_mm[Z_AXIS] = (target[Z_AXIS]-position[Z_AXIS])/axis_steps_per_unit[Z_AXIS];
  delta_mm[E_AXIS] = ((target[E_AXIS]-position[E_AXIS])/axis_steps_per_unit[E_AXIS + extruder]) *
                     extrudemultiply / 100.0;

  block->retract = block->restore = false;
  if ( block->steps_x <= dropsegments && block->steps_y <= dropsegments && block->steps_z <= dropsegments )
  {
    block->millimeters = fabs(delta_mm[E_AXIS]);
    no_move = true;
    // If retracting/returning mark the block as such
    if(block->steps_e != 0) {
      if((block->direction_bits & (1<<E_AXIS)) != 0) {
        block->retract = true;
      }
      else {
        block->restore = true;
      }
    }
  } 
  else
  {
    block->millimeters = sqrt(square(delta_mm[X_AXIS]) + square(delta_mm[Y_AXIS]) + square(delta_mm[Z_AXIS]));
    no_move = false;
  }
  float inverse_millimeters = 1.0/block->millimeters;  // Inverse millimeters to remove multiple divides 
  // Calculate speed in mm/second for each axis. No divide by zero due to previous checks.
  float inverse_second = feed_rate * inverse_millimeters;
  int moves_queued = num_blocks_queued();

#ifdef SLOWDOWN
  // Slow down only moves that are not retracting/returning and not moving Z
  if(delta_mm[E_AXIS]!=0 && delta_mm[Z_AXIS]==0 && (delta_mm[X_AXIS]!=0 || delta_mm[Y_AXIS]!=0)) 
  {
    //  segment time im micro seconds
    unsigned long segment_time = lround(1000000.0/inverse_second);
    if ((moves_queued > 1) && (moves_queued < (BLOCK_BUFFER_SIZE * 0.5)))
    {
      if (segment_time < minsegmenttime)
      { // buffer is draining, add extra time.  The amount of time added increases if the buffer is still emptied more.
        inverse_second=1000000.0/(segment_time+lround(2*(minsegmenttime-segment_time)/moves_queued));
        #ifdef XY_FREQUENCY_LIMIT
           segment_time = lround(1000000.0/inverse_second);
        #endif
      }
    }
  }
#endif // SLOWDOWN

  block->nominal_speed = block->millimeters * inverse_second; // (mm/sec) Always > 0
  block->nominal_rate = ceil(block->step_event_count * inverse_second); // (step/sec) Always > 0

  // Calculate and limit speed in mm/sec for each axis
  float current_speed[4];
  float speed_factor = 1.0; //factor <=1 do decrease speed
  for(int i=0; i < 3; i++) 
  {
    current_speed[i] = delta_mm[i] * inverse_second;
    if(fabs(current_speed[i]) > max_feedrate[i])
      speed_factor = min(speed_factor, max_feedrate[i] / fabs(current_speed[i]));
  }
  
#ifdef C_COMPENSATION
# define COMP_SPEED (gCCom_min_speed[extruder])
  block->advance_step_rate = axis_steps_per_unit[E_AXIS+extruder] * gCCom_min_speed[extruder];
  block->prev_advance = 0;
  block->next_advance = 0;
#else // C_COMPENSATION
# define COMP_SPEED (0.0)
#endif // C_COMPENSATION

  current_speed[E_AXIS] = delta_mm[E_AXIS] * inverse_second;
  if(fabs(current_speed[E_AXIS]) > max_feedrate[E_AXIS + extruder] - COMP_SPEED)
  {
    speed_factor = min(speed_factor, (max_feedrate[E_AXIS + extruder] - COMP_SPEED) / 
                                     fabs(current_speed[E_AXIS]));
  }
  
  // Max segement time in us.
#ifdef XY_FREQUENCY_LIMIT
#define MAX_FREQ_TIME (1000000.0/XY_FREQUENCY_LIMIT)
  // Check and limit the xy direction change frequency
  unsigned char direction_change = block->direction_bits ^ old_direction_bits;
  old_direction_bits = block->direction_bits;
  segment_time = lround((float)segment_time / speed_factor);
  
  if((direction_change & (1<<X_AXIS)) == 0)
  {
    x_segment_time[0] += segment_time;
  }
  else
  {
    x_segment_time[2] = x_segment_time[1];
    x_segment_time[1] = x_segment_time[0];
    x_segment_time[0] = segment_time;
  }
  if((direction_change & (1<<Y_AXIS)) == 0)
  {
    y_segment_time[0] += segment_time;
  }
  else
  {
    y_segment_time[2] = y_segment_time[1];
    y_segment_time[1] = y_segment_time[0];
    y_segment_time[0] = segment_time;
  }
  long max_x_segment_time = max(x_segment_time[0], max(x_segment_time[1], x_segment_time[2]));
  long max_y_segment_time = max(y_segment_time[0], max(y_segment_time[1], y_segment_time[2]));
  long min_xy_segment_time =min(max_x_segment_time, max_y_segment_time);
  if(min_xy_segment_time < MAX_FREQ_TIME)
    speed_factor = min(speed_factor, speed_factor * (float)min_xy_segment_time / (float)MAX_FREQ_TIME);
#endif

  // Correct the speed  
  if( speed_factor < 1.0)
  {
    for(unsigned char i=0; i < 4; i++)
    {
      current_speed[i] *= speed_factor;
    }
    block->nominal_speed *= speed_factor;
    block->nominal_rate *= speed_factor;
  }

  // Compute and limit the acceleration rate for the trapezoid generator.  
  float steps_per_mm = block->step_event_count/block->millimeters;
  if(no_move)
  {
    block->acceleration_st = ceil(retract_acceleration[extruder] * steps_per_mm); // convert to: acceleration steps/sec^2
  }
  else
  {
    block->acceleration_st = ceil(acceleration * steps_per_mm); // convert to: acceleration steps/sec^2
    // Calculate the acceleration in steps for each axis
    for(int i=0; i < NUM_AXIS; i++)
    {
      int ii = i + ((i==E_AXIS) ? extruder : 0);
      axis_steps_per_sqr_second[i] = max_acceleration_units_per_sq_second[ii] * 
                                     axis_steps_per_unit[ii];
    }
    // Limit acceleration per axis
    if(((float)block->acceleration_st * (float)block->steps_x / (float)block->step_event_count) > axis_steps_per_sqr_second[X_AXIS])
      block->acceleration_st = axis_steps_per_sqr_second[X_AXIS];
    if(((float)block->acceleration_st * (float)block->steps_y / (float)block->step_event_count) > axis_steps_per_sqr_second[Y_AXIS])
      block->acceleration_st = axis_steps_per_sqr_second[Y_AXIS];
    if(((float)block->acceleration_st * (float)block->steps_z / (float)block->step_event_count ) > axis_steps_per_sqr_second[Z_AXIS])
      block->acceleration_st = axis_steps_per_sqr_second[Z_AXIS];
    if(((float)block->acceleration_st * (float)block->steps_e / (float)block->step_event_count) > axis_steps_per_sqr_second[E_AXIS])
      block->acceleration_st = axis_steps_per_sqr_second[E_AXIS];
  }
  block->acceleration = block->acceleration_st / steps_per_mm;
  block->acceleration_rate = (long)((float)block->acceleration_st * 8.388608);


  // For E-only moves use the user defined max for E axis otherwise use XY max
  float safe_speed;
  if(no_move) {
     block->entry_speed = block->max_entry_speed = safe_speed = min(max_e_jerk[extruder], block->nominal_speed);
  }
  else
  {
    // Start with a safe speed
    float vmax_junction = max_xy_jerk/2; 
    float vmax_junction_factor = 1.0; 
    if(fabs(current_speed[Z_AXIS]) > max_z_jerk/2) 
      vmax_junction = min(vmax_junction, max_z_jerk/2);
    if(fabs(current_speed[E_AXIS]) > max_e_jerk[extruder]/2) 
      vmax_junction = min(vmax_junction, max_e_jerk[extruder]/2);
    vmax_junction = min(vmax_junction, block->nominal_speed);
    float safe_speed = vmax_junction;

    if ((moves_queued > 1) && (previous_nominal_speed > 0.0001)) {
      float jerk = sqrt(pow((current_speed[X_AXIS]-previous_speed[X_AXIS]), 2)+pow((current_speed[Y_AXIS]-previous_speed[Y_AXIS]), 2));
      vmax_junction = block->nominal_speed;
      if (jerk > max_xy_jerk) {
        vmax_junction_factor = (max_xy_jerk/jerk);
      } 
      if(fabs(current_speed[Z_AXIS] - previous_speed[Z_AXIS]) > max_z_jerk) {
        vmax_junction_factor= min(vmax_junction_factor, (max_z_jerk/fabs(current_speed[Z_AXIS] - previous_speed[Z_AXIS])));
      } 
      if(fabs(current_speed[E_AXIS] - previous_speed[E_AXIS]) + COMP_SPEED > max_e_jerk[extruder]) {
        vmax_junction_factor = min(vmax_junction_factor, (max_e_jerk[extruder]/(fabs(current_speed[E_AXIS] - previous_speed[E_AXIS])) + COMP_SPEED));
      } 
      vmax_junction = min(previous_nominal_speed, vmax_junction * vmax_junction_factor); // Limit speed to max previous speed
    }
    block->max_entry_speed = vmax_junction;

    // Initialize block entry speed. Compute based on deceleration to user-defined MINIMUM_PLANNER_SPEED.
    double v_allowable = max_allowable_speed(-block->acceleration,MINIMUM_PLANNER_SPEED,block->millimeters);
    block->entry_speed = min(vmax_junction, v_allowable);

#ifdef C_COMPENSATION
    // Bump up the compensation speed to e-jerk if can
    if(fabs(current_speed[E_AXIS] - previous_speed[E_AXIS]) + COMP_SPEED < max_e_jerk[extruder]) {
      block->advance_step_rate = axis_steps_per_unit[E_AXIS+extruder] * 
                                 (max_e_jerk[extruder] - fabs(current_speed[E_AXIS] - previous_speed[E_AXIS]));
    } 
#endif // C_COMPENSATION

    // Initialize planner efficiency flags
    // Set flag if block will always reach maximum junction speed regardless of entry/exit speeds.
    // If a block can de/ac-celerate from nominal speed to zero within the length of the block, then
    // the current block and next block junction speeds are guaranteed to always be at their maximum
    // junction speeds in deceleration and acceleration, respectively. This is due to how the current
    // block nominal speed limits both the current and next maximum junction speeds. Hence, in both
    // the reverse and forward planners, the corresponding block junction speed will always be at the
    // the maximum junction speed and may always be ignored for any speed reduction checks.
    if (block->nominal_speed <= v_allowable) { 
      block->nominal_length_flag = true; 
    }
    else { 
      block->nominal_length_flag = false; 
    }
    block->recalculate_flag = true; // Always calculate trapezoid for new block
  }
  
  calculate_trapezoid_for_block(block, block->entry_speed/block->nominal_speed,
                                safe_speed/block->nominal_speed);

  // Update previous path unit_vector and nominal speed
  memcpy(previous_speed, current_speed, sizeof(previous_speed)); // previous_speed[] = current_speed[]
  previous_nominal_speed = block->nominal_speed;

  // Move buffer head
  block_buffer_head = next_buffer_head;

  // Update position
  memcpy(position, target, sizeof(target)); // position[] = target[]

  // Recalculate
  planner_recalculate();

  st_wake_up();
}

void plan_set_position(const float &x, const float &y, const float &z, const float &e)
{
  position[X_AXIS] = lround(x*axis_steps_per_unit[X_AXIS]);
  position[Y_AXIS] = lround(y*axis_steps_per_unit[Y_AXIS]);
  position[Z_AXIS] = lround(z*axis_steps_per_unit[Z_AXIS]);     
  position[E_AXIS] = lround(e*axis_steps_per_unit[E_AXIS + ACTIVE_EXTRUDER]);
  last_extruder = ACTIVE_EXTRUDER;
  st_set_position(position[X_AXIS], position[Y_AXIS], position[Z_AXIS], position[E_AXIS]);
  previous_nominal_speed = 0.0; // Resets planner junction speeds. Assumes start from rest.
  previous_speed[0] = 0.0;
  previous_speed[1] = 0.0;
  previous_speed[2] = 0.0;
  previous_speed[3] = 0.0;
}

void plan_set_e_position(const float &e)
{
  position[E_AXIS] = lround(e*axis_steps_per_unit[E_AXIS + ACTIVE_EXTRUDER]);
  last_extruder = ACTIVE_EXTRUDER;
  st_set_e_position(position[E_AXIS]);
}

uint8_t movesplanned()
{
  return (block_buffer_head-block_buffer_tail + BLOCK_BUFFER_SIZE) & (BLOCK_BUFFER_SIZE - 1);
}

void allow_cold_extrudes(bool allow)
{
#ifdef PREVENT_DANGEROUS_EXTRUDE
  allow_cold_extrude=allow;
#endif
}

