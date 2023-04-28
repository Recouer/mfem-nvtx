FILES=(fem/bilinearform.cpp fem/bilininteg.cpp fem/linearform.cpp linalg/hypre.cpp linalg/operator.cpp linalg/petsc.cpp linalg/solvers.cpp linalg/sparsemat.cpp linalg/sparsesmoothers.cpp linalg/vector.cpp linalg/vector.hpp)

for file in ${FILES[@]}
do
    sed -i 's/nvtxRangePush(__FUNCTION__);/char str_nvtx[256];\n\tsprintf(str_nvtx, "%d ", __LINE__);\n\tstrcat(str_nvtx, __FILE__);\n\tstrcat(str_nvtx, " ");\n\tstrcat(str_nvtx, __FUNCTION__);\n\tnvtxRangePush(str_nvtx);/g' $file
done

