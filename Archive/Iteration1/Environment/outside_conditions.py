"""
Outside environmental conditions for greenhouse simulation.

Provides temperature and humidity profiles based on historical weather data
from May 1 to September 1, 2025.

Data source: airTemperature_2025-05-01_to_2025-09-01.csv
"""

import csv
from datetime import datetime, timedelta
from pathlib import Path
from typing import Tuple, Optional
import bisect


class OutsideConditions:
    """
    Manages outside temperature and humidity profiles for simulation.
    
    Provides interpolated values for:
    - T_out(t): Outside air temperature [°C]
    - RH_out(t): Outside relative humidity [%]
    """
    
    def __init__(self, csv_path: Optional[Path] = None):
        """
        Load environmental data from CSV file.
        
        Args:
            csv_path: Path to CSV file. If None, uses default location.
        """
        if csv_path is None:
            csv_path = Path(__file__).parent / "airTemperature_2025-05-01_to_2025-09-01.csv"
        
        self.timestamps = []  # List of datetime objects
        self.temperatures = []  # List of temperatures [°C]
        self.humidities = []  # List of relative humidities [%]
        
        self._load_data(csv_path)
        
        # Store start time for relative time calculations
        self.start_time = self.timestamps[0]
        self.end_time = self.timestamps[-1]
        self.duration_seconds = (self.end_time - self.start_time).total_seconds()
    
    def _load_data(self, csv_path: Path):
        """Load data from CSV file."""
        with open(csv_path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                # Parse timestamp
                dt = datetime.strptime(row['dateTime'], '%Y-%m-%d %H:%M:%S')
                self.timestamps.append(dt)
                
                # Parse temperature and humidity
                self.temperatures.append(float(row['airTemperature']))
                self.humidities.append(float(row['airHumidity']))
        
        print(f"Loaded {len(self.timestamps)} data points")
        print(f"Date range: {self.timestamps[0]} to {self.timestamps[-1]}")
        print(f"Temperature range: {min(self.temperatures):.1f}°C to {max(self.temperatures):.1f}°C")
        print(f"Humidity range: {min(self.humidities):.1f}% to {max(self.humidities):.1f}%")
    
    def get_conditions_at_time(self, t: datetime) -> Tuple[float, float]:
        """
        Get interpolated outside conditions at a specific datetime.
        
        Args:
            t: Datetime for which to get conditions
            
        Returns:
            (T_out, RH_out): Temperature [°C] and relative humidity [%]
        """
        # Handle out-of-range times by clamping to dataset bounds
        if t <= self.start_time:
            return self.temperatures[0], self.humidities[0]
        if t >= self.end_time:
            return self.temperatures[-1], self.humidities[-1]
        
        # Find surrounding data points using binary search
        idx = bisect.bisect_left(self.timestamps, t)
        
        # If exact match
        if idx < len(self.timestamps) and self.timestamps[idx] == t:
            return self.temperatures[idx], self.humidities[idx]
        
        # Interpolate between idx-1 and idx
        t0 = self.timestamps[idx - 1]
        t1 = self.timestamps[idx]
        T0 = self.temperatures[idx - 1]
        T1 = self.temperatures[idx]
        RH0 = self.humidities[idx - 1]
        RH1 = self.humidities[idx]
        
        # Linear interpolation
        dt_total = (t1 - t0).total_seconds()
        dt_elapsed = (t - t0).total_seconds()
        alpha = dt_elapsed / dt_total
        
        T_out = T0 + alpha * (T1 - T0)
        RH_out = RH0 + alpha * (RH1 - RH0)
        
        return T_out, RH_out
    
    def get_conditions_at_elapsed_time(self, t_seconds: float) -> Tuple[float, float]:
        """
        Get interpolated outside conditions at elapsed time since start.
        
        Args:
            t_seconds: Elapsed time in seconds since simulation start
            
        Returns:
            (T_out, RH_out): Temperature [°C] and relative humidity [%]
        """
        # Calculate absolute datetime
        dt = self.start_time + timedelta(seconds=t_seconds)
        return self.get_conditions_at_time(dt)
    
    def get_conditions_cyclic(self, t_seconds: float, cycle_days: int = 1) -> Tuple[float, float]:
        """
        Get interpolated conditions with cyclic repetition.
        
        Useful for simulations longer than the dataset. The profile repeats
        every cycle_days days.
        
        Args:
            t_seconds: Elapsed time in seconds since simulation start
            cycle_days: Number of days from dataset to use as one cycle
            
        Returns:
            (T_out, RH_out): Temperature [°C] and relative humidity [%]
        """
        cycle_seconds = cycle_days * 86400  # days to seconds
        t_cyclic = t_seconds % cycle_seconds
        return self.get_conditions_at_elapsed_time(t_cyclic)
    
    def get_day_profile(self, day_offset: int = 0) -> Tuple[list, list]:
        """
        Extract a complete day profile starting from the first day + offset.
        
        Args:
            day_offset: Number of days from the start of the dataset
            
        Returns:
            (times, temperatures, humidities): Lists of elapsed time [s] from
            day start, temperatures [°C], and humidities [%]
        """
        day_start = self.start_time + timedelta(days=day_offset)
        day_end = day_start + timedelta(days=1)
        
        times = []
        temps = []
        hums = []
        
        for i, ts in enumerate(self.timestamps):
            if day_start <= ts < day_end:
                elapsed = (ts - day_start).total_seconds()
                times.append(elapsed)
                temps.append(self.temperatures[i])
                hums.append(self.humidities[i])
        
        return times, temps, hums


# Create a global instance for convenience
_global_conditions = None


def get_outside_conditions(csv_path: Optional[Path] = None) -> OutsideConditions:
    """
    Get or create the global OutsideConditions instance.
    
    Args:
        csv_path: Path to CSV file (only used on first call)
        
    Returns:
        OutsideConditions instance
    """
    global _global_conditions
    if _global_conditions is None:
        _global_conditions = OutsideConditions(csv_path)
    return _global_conditions


# Convenience functions for direct use
def T_out(t: float) -> float:
    """
    Get outside temperature at elapsed time t.
    
    Args:
        t: Elapsed time in seconds since simulation start
        
    Returns:
        Outside temperature [°C]
    """
    conditions = get_outside_conditions()
    T, _ = conditions.get_conditions_at_elapsed_time(t)
    return T


def RH_out(t: float) -> float:
    """
    Get outside relative humidity at elapsed time t.
    
    Args:
        t: Elapsed time in seconds since simulation start
        
    Returns:
        Outside relative humidity [%]
    """
    conditions = get_outside_conditions()
    _, RH = conditions.get_conditions_at_elapsed_time(t)
    return RH


def get_conditions(t: float) -> Tuple[float, float]:
    """
    Get both outside temperature and humidity at elapsed time t.
    
    Args:
        t: Elapsed time in seconds since simulation start
        
    Returns:
        (T_out, RH_out): Temperature [°C] and relative humidity [%]
    """
    conditions = get_outside_conditions()
    return conditions.get_conditions_at_elapsed_time(t)


if __name__ == "__main__":
    # Example usage and testing
    import matplotlib.pyplot as plt
    
    print("Loading outside conditions data...")
    conditions = OutsideConditions()
    
    print("\n--- Test 1: Sample conditions at specific times ---")
    # Test at start of day 1
    T, RH = conditions.get_conditions_at_elapsed_time(0)
    print(f"t=0s (start): T={T:.1f}°C, RH={RH:.1f}%")
    
    # Test at noon of day 1 (approximately 43200 seconds)
    T, RH = conditions.get_conditions_at_elapsed_time(43200)
    print(f"t=43200s (noon): T={T:.1f}°C, RH={RH:.1f}%")
    
    # Test at end of day 1
    T, RH = conditions.get_conditions_at_elapsed_time(86400)
    print(f"t=86400s (24h): T={T:.1f}°C, RH={RH:.1f}%")
    
    print("\n--- Test 2: Extract first day profile and plot ---")
    times, temps, hums = conditions.get_day_profile(day_offset=0)
    
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)
    
    # Convert times to hours for plotting
    times_hours = [t / 3600 for t in times]
    
    ax1.plot(times_hours, temps, 'r-', linewidth=2)
    ax1.set_ylabel('Outside Temperature [°C]', fontsize=12)
    ax1.grid(True, alpha=0.3)
    ax1.set_title('Outside Environmental Conditions - May 1, 2025', fontsize=14, fontweight='bold')
    
    ax2.plot(times_hours, hums, 'b-', linewidth=2)
    ax2.set_xlabel('Time [hours]', fontsize=12)
    ax2.set_ylabel('Outside Relative Humidity [%]', fontsize=12)
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig('outside_conditions_day1.png', dpi=150)
    print("Saved plot to 'outside_conditions_day1.png'")
    
    print("\n--- Test 3: Cyclic repetition ---")
    # Test that cyclic mode works (simulate 2.5 days using 1-day cycle)
    T1, RH1 = conditions.get_conditions_cyclic(0, cycle_days=1)
    T2, RH2 = conditions.get_conditions_cyclic(86400, cycle_days=1)  # +1 day
    T3, RH3 = conditions.get_conditions_cyclic(2*86400 + 43200, cycle_days=1)  # +2.5 days
    
    print(f"Cyclic t=0s: T={T1:.1f}°C, RH={RH1:.1f}%")
    print(f"Cyclic t=1day: T={T2:.1f}°C, RH={RH2:.1f}% (should match t=0)")
    print(f"Cyclic t=2.5days: T={T3:.1f}°C, RH={RH3:.1f}% (should match t=0.5day)")
    
    print("\nDone!")
