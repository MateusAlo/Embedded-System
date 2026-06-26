import sys
import time
import serial
import serial.tools.list_ports
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque

def find_esp32_port():
    print("Searching for ESP32 on serial ports (ttyACM* or ttyUSB*)...")
    while True:
        ports = serial.tools.list_ports.comports()
        for port in ports:
            # Look for common ESP32 USB-to-Serial converter names or simply ACM/USB
            if "ttyACM" in port.device or "ttyUSB" in port.device:
                try:
                    # Test if we can open it
                    ser = serial.Serial(port.device, 115200, timeout=0.1)
                    print(f"Successfully connected to {port.device} at 115200 baud.")
                    return ser
                except serial.SerialException:
                    # Port might be busy
                    pass
        time.sleep(1)

def main():
    ser = find_esp32_port()

    # Data structures for plotting
    MAX_POINTS = 200
    y_data = deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS)
    x_data = list(range(MAX_POINTS))
    
    # Plot setup
    plt.style.use('dark_background')
    fig, ax = plt.subplots(figsize=(10, 6))
    line, = ax.plot(x_data, y_data, color='cyan', linewidth=2)
    
    ax.set_title('Real-time IMU Path Data (First Value)', fontsize=14, color='white')
    ax.set_xlabel('Samples', fontsize=12)
    ax.set_ylabel('Value', fontsize=12)
    ax.grid(True, linestyle='--', alpha=0.3)
    
    # Loop time text display in the top-left corner
    loop_time_text = ax.text(0.02, 0.95, 'LOOP_TIME: -- ms', 
                             transform=ax.transAxes, 
                             fontsize=14, 
                             color='yellow', 
                             bbox=dict(facecolor='black', alpha=0.7, edgecolor='white'))

    # Initial Y axis limits
    ax.set_ylim(-0.05, 0.05)

    def update(frame):
        # Read all available lines from serial buffer
        lines_read = 0
        while ser.in_waiting and lines_read < 100: # Limit reads to avoid blocking UI forever
            try:
                # Read a line and decode it
                line_str = ser.readline().decode('utf-8', errors='ignore').strip()
                
                # Check if it's the data line we care about
                if line_str.startswith("PATH_DATA:"):
                    # Example: PATH_DATA: 0.005,0.000,0.001,0.295 | LOOP_TIME: 70.0 ms
                    parts = line_str.split("|")
                    if len(parts) == 2:
                        path_str = parts[0].replace("PATH_DATA:", "").strip()
                        loop_time_str = parts[1].replace("LOOP_TIME:", "").strip()
                        
                        # Extract the first number
                        first_num_str = path_str.split(",")[0]
                        try:
                            value = float(first_num_str)
                            y_data.append(value)
                            
                            # Update loop time text
                            loop_time_text.set_text(f'LOOP_TIME: {loop_time_str}')
                        except ValueError:
                            pass # In case parsing fails
            except Exception as e:
                pass
            lines_read += 1
            
        # Update the plot line
        line.set_ydata(y_data)
        
        # Auto-adjust Y axis dynamically if data goes out of bounds
        current_min = min(y_data)
        current_max = max(y_data)
        y_lim_min, y_lim_max = ax.get_ylim()
        
        # Add a 20% margin to the current min/max
        margin = max(abs(current_max - current_min) * 0.2, 0.02)
        
        target_min = current_min - margin
        target_max = current_max + margin

        # Smoothly adjust the limits or jump if strictly out of bounds
        new_min = min(y_lim_min, target_min)
        new_max = max(y_lim_max, target_max)
        
        # If the data has been small for a while, shrink the bounds
        if target_max < y_lim_max - margin * 2 and target_min > y_lim_min + margin * 2:
             new_max = target_max
             new_min = target_min

        if new_min != y_lim_min or new_max != y_lim_max:
            ax.set_ylim(new_min, new_max)
            
        return line, loop_time_text

    # Set interval to 20ms (~50 FPS)
    ani = animation.FuncAnimation(fig, update, interval=20, blit=False, cache_frame_data=False)
    
    try:
        plt.tight_layout()
        plt.show()
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
