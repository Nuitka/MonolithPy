import sys
import __np__

def main():
    module_file = sys.argv[1]
    __np__.rename_init_symbol_in_file(module_file)

if __name__ == "__main__":
    main()