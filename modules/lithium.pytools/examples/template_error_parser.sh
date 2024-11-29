# Example 1: Parse an error log file and generate a visual error tree
# This example parses the specified error log file, generates a visual error tree in PNG format, and opens the image for viewing.
$ python template_error_parser.py error_log.txt --graph error_tree --format png --view

# Example 2: Extract code context around the error
# This example parses the specified error log file and extracts 5 lines of code before and after the error location.
$ python template_error_parser.py error_log.txt --context 5

# Example 3: Download and analyze an error log file
# This example downloads the error log file from the provided URL, parses it, and saves the results.
$ python template_error_parser.py downloaded_log.txt --download http://example.com/error_log.txt

# Example 4: Decompress and analyze a gzip error log file
# This example decompresses the specified gzip error log file, parses the decompressed file, and saves the results.
$ python template_error_parser.py decompressed_error.txt --decompress error_log.txt.gz

# Example 5: Parse an error log file from a specific compiler
# This example parses the specified error log file generated by Clang and generates a visual error tree in PDF format.
$ python template_error_parser.py error_log.txt --compiler clang --graph error_tree --format pdf

# Example 6: Parse an error log file without saving the generated graph
# This example parses the specified error log file and generates a visual error tree without saving the graph file.
$ python template_error_parser.py error_log.txt --graph error_tree --no-save

# Example 7: Save parsed error information to a custom JSON file
# This example parses the specified error log file and saves the parsed error information to a custom JSON file.
$ python template_error_parser.py error_log.txt --output custom_parsed_error.json

# Example 8: Parse an error log file and generate a visual error tree without viewing
# This example parses the specified error log file and generates a visual error tree in PNG format without opening the image for viewing.
$ python template_error_parser.py error_log.txt --graph error_tree --format png

# Example 9: Parse an error log file and generate a visual error tree in SVG format
# This example parses the specified error log file and generates a visual error tree in SVG format.
$ python template_error_parser.py error_log.txt --graph error_tree --format svg

# Example 10: Parse an error log file and generate a visual error tree with a custom name
# This example parses the specified error log file and generates a visual error tree with a custom name in PNG format.
$ python template_error_parser.py error_log.txt --graph custom_error_tree --format png